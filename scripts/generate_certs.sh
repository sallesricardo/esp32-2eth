#!/usr/bin/env bash
# Gera uma CA própria e assina certificados para o broker (PC) e o device
# (ESP32) -- pensado para o par fixo PC <-> ESP32 deste projeto, não para
# uma frota (pra fleet, considere provisionamento por device via NVS em vez
# de embutir a chave no binário).
#
# Uso:
#   BROKER_IP=192.168.1.100 CLIENT_CN=esp32-device-01 ./generate_certs.sh
#
# Rode isso no seu PC, NUNCA num ambiente compartilhado -- a chave privada
# gerada aqui (ca.key, client.key) é segredo.

set -euo pipefail

BROKER_IP="${BROKER_IP:-192.168.1.100}"   # IP do PC/broker na rede onde o ESP32 conecta
CLIENT_CN="${CLIENT_CN:-esp32-device-01}" # vira o "usuario" no Mosquitto via use_identity_as_username
DAYS_VALID="${DAYS_VALID:-3650}"          # 10 anos -- par fixo, sem infra de renovação automática

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../out/certs"

mkdir -p "$OUT_DIR"
cd "$OUT_DIR"

echo "==> Gerando CA propria (out/certs/ca.key, ca.crt)..."
openssl genrsa -out ca.key 4096
openssl req -x509 -new -nodes -key ca.key -sha256 -days "$DAYS_VALID" \
    -subj "/CN=proxy-mtls-ca" \
    -out ca.crt

echo "==> Gerando certificado do broker (CN/SAN = $BROKER_IP)..."
openssl genrsa -out broker.key 2048
openssl req -new -key broker.key -subj "/CN=$BROKER_IP" -out broker.csr
openssl x509 -req -in broker.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -days "$DAYS_VALID" -sha256 \
    -extfile <(printf "subjectAltName=IP:%s" "$BROKER_IP") \
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
