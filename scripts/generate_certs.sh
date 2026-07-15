#!/usr/bin/env bash
# Gera uma CA própria e assina certificados para o broker (PC) e o device
# (ESP32) -- pensado para o par fixo PC <-> ESP32 deste projeto, não para
# uma frota (pra fleet, considere provisionamento por device via NVS em vez
# de embutir a chave no binário).
#
# Uso (1 IP):
#   BROKER_IP=192.168.1.100 CLIENT_CN=esp32-device-01 ./generate_certs.sh
#
# Uso (vários IPs -- ex: PC com mais de uma interface/rede acessando o
# broker, ou você ainda não decidiu qual IP vai usar em produção):
#   BROKER_IPS="192.168.1.100,192.168.2.1" CLIENT_CN=esp32-device-01 ./generate_certs.sh
#
# Se alguma app vai conectar via hostname em vez de IP (ex: "localhost"),
# use BROKER_DNS_NAMES (separado por vírgula) -- entradas de hostname vão
# pro SAN como DNS:, que é o que a validação TLS usa quando a URI de
# conexão é um nome, não um literal de IP:
#   BROKER_IPS="192.168.1.100,127.0.0.1" BROKER_DNS_NAMES="localhost" ./generate_certs.sh
#
# Todo IP/host que algum dia for usado em CONFIG_MQTT_BROKER_URI (ou na URI
# de qualquer outro cliente mTLS) precisa estar num desses dois -- o
# mbedTLS/OpenSSL validam o host da URI contra o SAN do certificado do
# broker, e derrubam a conexão TLS se não achar correspondência.
#
# Rodar de novo com BROKER_IP(S)/BROKER_DNS_NAMES diferentes REAPROVEITA a
# CA existente (out/certs/ca.key/ca.crt) se ela já existir -- só reemite o
# broker.crt. Isso é de propósito: trocar o certificado do broker não pode
# invalidar o ca.crt já embutido no firmware do ESP32 nem os certificados
# de outros clientes já emitidos. Pra criar um certificado de CLIENTE NOVO
# (outra app, outro device) reaproveitando essa mesma CA, use
# scripts/generate_client_cert.sh -- não rode este script de novo pra isso.
#
# Rode isso no seu PC, NUNCA num ambiente compartilhado -- a chave privada
# gerada aqui (ca.key, client.key) é segredo.

set -euo pipefail

BROKER_IP="${BROKER_IP:-192.168.1.100}"   # IP do PC/broker na rede onde o ESP32 conecta
BROKER_IPS="${BROKER_IPS:-$BROKER_IP}"    # lista separada por vírgula; BROKER_IP vira o 1o item se BROKER_IPS não for setado
BROKER_DNS_NAMES="${BROKER_DNS_NAMES:-}"  # opcional, separado por vírgula (ex: "localhost")
CLIENT_CN="${CLIENT_CN:-esp32-device-01}" # vira o "usuario" no Mosquitto via use_identity_as_username
DAYS_VALID="${DAYS_VALID:-3650}"          # 10 anos -- par fixo, sem infra de renovação automática

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../out/certs"

mkdir -p "$OUT_DIR"
cd "$OUT_DIR"

# Monta "IP:192.168.1.100,IP:127.0.0.1,DNS:localhost,..." a partir de
# BROKER_IPS + BROKER_DNS_NAMES
SAN_LIST=""
append_san() {
    local prefix="$1" val
    val="$(echo "$2" | xargs)" # trim de espaços
    [ -z "$val" ] && return
    if [ -n "$SAN_LIST" ]; then
        SAN_LIST="${SAN_LIST},${prefix}:${val}"
    else
        SAN_LIST="${prefix}:${val}"
    fi
}
IFS=',' read -ra IP_ARRAY <<< "$BROKER_IPS"
for ip in "${IP_ARRAY[@]}"; do append_san "IP" "$ip"; done
if [ -n "$BROKER_DNS_NAMES" ]; then
    IFS=',' read -ra DNS_ARRAY <<< "$BROKER_DNS_NAMES"
    for dns in "${DNS_ARRAY[@]}"; do append_san "DNS" "$dns"; done
fi

if [ -f ca.key ] && [ -f ca.crt ]; then
    echo "==> CA já existe (out/certs/ca.key/ca.crt) -- reaproveitando, NÃO regenerando."
    echo "    (isso mantém válido tudo que já foi assinado com ela: certificado"
    echo "     do ESP32, de outros clientes, e o ca.crt já embutido no firmware)"
else
    echo "==> Gerando CA propria (out/certs/ca.key, ca.crt)..."
    openssl genrsa -out ca.key 4096
    openssl req -x509 -new -nodes -key ca.key -sha256 -days "$DAYS_VALID" \
        -subj "/CN=proxy-mtls-ca" \
        -out ca.crt
fi

echo "==> Gerando certificado do broker (SAN = $SAN_LIST)..."
openssl genrsa -out broker.key 2048
openssl req -new -key broker.key -subj "/CN=${IP_ARRAY[0]}" -out broker.csr
openssl x509 -req -in broker.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -days "$DAYS_VALID" -sha256 \
    -extfile <(printf "subjectAltName=%s" "$SAN_LIST") \
    -out broker.crt

echo "==> Gerando certificado do ESP32 (CN = $CLIENT_CN)..."
openssl genrsa -out client.key 2048
openssl req -new -key client.key -subj "/CN=$CLIENT_CN" -out client.csr
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -days "$DAYS_VALID" -sha256 \
    -out client.crt

rm -f broker.csr client.csr

echo
echo "==> Prontos em: $OUT_DIR"
echo "    ca.crt                 -> broker (Mosquitto) E firmware do ESP32"
echo "    ca.key                 -> GUARDE em local seguro; NAO precisa ir pro broker rodando nem pro ESP32"
echo "    broker.crt/broker.key  -> Mosquitto (ex: /etc/mosquitto/certs/)"
echo "    client.crt/client.key  -> firmware do ESP32"
echo
echo "Proximos passos:"
echo "  cp \"$OUT_DIR/ca.crt\" \"$OUT_DIR/client.crt\" \"$OUT_DIR/client.key\" \\"
echo "     \"$SCRIPT_DIR/../components/mqtt_app/certs/\""
echo "  # copie ca.crt, broker.crt, broker.key para onde o Mosquitto vai ler (veja mosquitto/mosquitto.conf.example)"
echo "  # ajuste 'user $CLIENT_CN' em mosquitto/acl.example se usar outro CLIENT_CN"
