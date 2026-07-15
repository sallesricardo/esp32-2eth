#!/usr/bin/env bash
# Emite um certificado de CLIENTE NOVO (outra app, outro device) assinado
# pela CA já existente -- sem tocar no broker nem gerar uma CA nova.
#
# Use isso pra qualquer cliente mTLS adicional que precise falar com o
# mesmo Mosquitto do ESP32: uma aplicação rodando no PC do broker, um
# segundo ESP32, uma ferramenta de debug, etc. Rodar scripts/generate_certs.sh
# de novo pra isso é o jeito ERRADO -- ele recriaria a CA do zero e
# invalidaria o ca.crt já embutido no firmware e os certs já emitidos.
#
# Pré-requisito: já ter rodado scripts/generate_certs.sh pelo menos uma vez
# (precisa existir out/certs/ca.key e out/certs/ca.crt).
#
# Uso:
#   CLIENT_CN=pc-app-01 ./scripts/generate_client_cert.sh
#
# Depois de gerar, falta:
#   1. Copiar ca.crt + <CLIENT_CN>.crt + <CLIENT_CN>.key pra onde a nova
#      aplicação vai ler os certificados.
#   2. Adicionar as permissões dela em mosquitto/acl.conf -- ver exemplo
#      abaixo, impresso no final deste script.
#   3. Reiniciar o mosquitto (ele não recarrega ACL/CA sozinho):
#        systemctl restart mosquitto
#      (não precisa mexer no ESP32 -- a CA não mudou.)

set -euo pipefail

CLIENT_CN="${CLIENT_CN:?defina CLIENT_CN=<nome-do-cliente>, ex: CLIENT_CN=pc-app-01 ./generate_client_cert.sh}"
DAYS_VALID="${DAYS_VALID:-3650}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../out/certs"

if [ ! -f "$OUT_DIR/ca.key" ] || [ ! -f "$OUT_DIR/ca.crt" ]; then
    echo "ERRO: $OUT_DIR/ca.key ou ca.crt não encontrados." >&2
    echo "Rode scripts/generate_certs.sh primeiro -- ele cria a CA." >&2
    exit 1
fi

cd "$OUT_DIR"

OUT_KEY="${CLIENT_CN}.key"
OUT_CSR="${CLIENT_CN}.csr"
OUT_CRT="${CLIENT_CN}.crt"

if [ -f "$OUT_CRT" ]; then
    echo "AVISO: $OUT_DIR/$OUT_CRT já existe -- será sobrescrito." >&2
fi

echo "==> Gerando certificado de cliente (CN=$CLIENT_CN), assinado pela CA existente..."
openssl genrsa -out "$OUT_KEY" 2048
openssl req -new -key "$OUT_KEY" -subj "/CN=$CLIENT_CN" -out "$OUT_CSR"
openssl x509 -req -in "$OUT_CSR" -CA ca.crt -CAkey ca.key -CAcreateserial \
    -days "$DAYS_VALID" -sha256 \
    -out "$OUT_CRT"

rm -f "$OUT_CSR"

echo
echo "==> Pronto: $OUT_DIR/$OUT_KEY, $OUT_DIR/$OUT_CRT"
echo "    ca.crt continua o mesmo de sempre -- não precisa reflashar o ESP32"
echo "    nem trocar o cafile do Mosquitto."
echo
echo "Falta:"
echo "  1. Copiar ca.crt, $OUT_CRT, $OUT_KEY pra onde a nova aplicação vai ler."
echo "  2. Adicionar em mosquitto/acl.conf (ajuste os tópicos pro que essa"
echo "     app realmente precisa -- o Mosquitto nega por padrão tudo que"
echo "     não estiver listado, quando acl_file está configurado):"
echo
echo "       user $CLIENT_CN"
echo "       topic read  device/tcp_server/client_connected"
echo
echo "  3. systemctl restart mosquitto"
