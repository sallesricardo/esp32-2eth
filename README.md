# Estrutura reorganizada

```
components/
  eth_w5500/          # driver genérico de uma porta W5500 (SPI + MAC/PHY + netif)
  mqtt_app/            # cliente MQTT com mTLS, amarrado a um netif específico
    certs/              # PLACEHOLDERS -- substitua pelos gerados em scripts/generate_certs.sh
  tcp_server_app/      # servidor TCP simples (1 conexão por vez)
  tcp_client_app/      # cliente TCP que conecta a um host remoto, com reconexão automática
  proxy_events/        # event loop dedicado pro processamento (desacoplado do I/O de rede)
main/
  app_main.c           # só orquestra: chama os 5 componentes acima e faz o proxy TCP
  Kconfig.projbuild    # opção de menuconfig pra fixar o proxy numa interface (ver seção abaixo)
scripts/
  generate_certs.sh    # gera CA própria + certs do broker e do ESP32
mosquitto/
  mosquitto.conf.example  # config de exemplo do broker (mTLS)
  acl.example              # ACL de exemplo restringindo tópicos do device
```

## Proxy TCP (tcp_server_app <-> tcp_client_app) via event loop dedicado

O `tcp_client_app` conecta como cliente TCP a `REMOTE_HOST_IP:REMOTE_HOST_PORT`
(hoje `192.168.1.100:50000`, definido em `main/app_main.c` — **ajuste pro IP
real**), reconectando automaticamente a cada 3s se a conexão cair.

**Ambas as pontas do proxy são fixadas via `idf.py menuconfig`** — em
"Dual W5500 App Configuration" → "Indice da interface Ethernet para fixar
o proxy TCP" (`CONFIG_TCP_PROXY_BIND_ETH_IDX`):

- `0` = ETH1 (normalmente reservado ao MQTT)
- `1` = ETH2 (normalmente reservado ao proxy TCP) — **valor default**
- `-1` = automático: não fixa em nenhuma interface, deixa a tabela de
  rotas do lwIP decidir (comportamento antigo)

Em código (`main/app_main.c`), isso é resolvido através de um array
indexado:

```c
esp_netif_t *eth_netifs[] = { eth_netif_1, eth_netif_2 };
esp_netif_t *proxy_bind_netif =
    (CONFIG_TCP_PROXY_BIND_ETH_IDX >= 0) ? eth_netifs[CONFIG_TCP_PROXY_BIND_ETH_IDX] : NULL;
```

`proxy_bind_netif` é então passado tanto pro `tcp_server_app_start` quanto
pro `tcp_client_app_start`. Com o default (`1` = ETH2):

- O servidor só aceita conexões que chegam fisicamente por ETH2 (bind no IP
  de ETH2 em vez de `INADDR_ANY`), mesmo que o host que conecta também
  esteja alcançável por ETH1.
- O cliente sai sempre por ETH2 (bind no IP de ETH2 antes do `connect()`),
  independente de qual sub-rede o host remoto esteja e de qual interface a
  tabela de rotas escolheria por padrão.

Sem essa fixação (`-1`), como o servidor originalmente fazia bind em
`INADDR_ANY` e o cliente não especificava interface de saída, os dois
sockets do proxy ficariam sujeitos à tabela de rotas do lwIP — o que na
prática significa que o tráfego do proxy poderia vazar pra ETH1 (a
interface reservada pro MQTT) dependendo de onde o host remoto estivesse
na rede, sem nenhum erro visível, só um comportamento diferente do
esperado.

**Se adicionar um 3º W5500 no futuro**: estenda o array `eth_netifs[]` em
`main/app_main.c` e o `range` da opção `TCP_PROXY_BIND_ETH_IDX` em
`main/Kconfig.projbuild` (hoje `-1 1`, viraria `-1 2`).

**Nota sobre `main/Kconfig.projbuild`**: este projeto já referenciava
`CONFIG_ETH1_*`, `CONFIG_ETH2_*`, `CONFIG_MQTT_BROKER_URI` e
`CONFIG_TCP_SERVER_PORT`, que presumo já existirem no seu
`Kconfig.projbuild` real (não incluído neste zip, já que não fazia parte
do código-fonte original enviado). O arquivo `main/Kconfig.projbuild`
incluído aqui só tem a nova opção `TCP_PROXY_BIND_ETH_IDX` — **mescle
com o seu arquivo existente** em vez de substituí-lo, ou o build vai
reclamar de opções indefinidas (`CONFIG_ETH1_STATIC_IP_ADDR` etc.).

O processamento dos dados roda num **event loop dedicado** (`proxy_events`),
separado do loop default usado por `ETH_EVENT`/`IP_EVENT`. Isso significa
uma task própria só pra processamento, desacoplada das tasks de I/O de rede:

```
[task tcp_server_app]  --recv()-->  on_data_from_tcp_client()
                                       -> proxy_events_post_data(PROXY_EVENT_DATA_FROM_TCP_CLIENT, ...)
                                            (copia os bytes pro heap, tira timestamp, enfileira)
                                                    |
                                                    v
                                    [task proxy_evt_loop] -> handle_data_from_tcp_client_event()
                                                                -> process_data_from_tcp_client(data, len, latencia)
                                                                -> tcp_client_app_send(data, len)  // proxy
                                                                -> free(data)
```

(o caminho inverso, do `tcp_client_app` pro `tcp_server_app`, é simétrico,
via `PROXY_EVENT_DATA_FROM_REMOTE_HOST`).

**Por que separar assim**: os callbacks `on_data_from_tcp_client` /
`on_data_from_remote_host`, que rodam *dentro* da task que fez o `recv()`,
ficam propositalmente leves — só copiam o buffer e enfileiram. Processamento
pesado (parsing, gravação em flash, etc.) fica isolado na task
`proxy_evt_loop` e nunca atrasa a leitura do socket. Como bônus, esse
desenho já resolve o descompasso de velocidade entre interfaces de rede
diferentes (ex: se um dos lados virar RMII e ficar bem mais rápido que o
outro em SPI/W5500) sem travar a task de recepção — a fila do event loop
absorve rajadas até `PROXY_EVENT_QUEUE_SIZE` (16, em `proxy_events.c`)
eventos pendentes.

**Latência**: cada `proxy_data_event_t` carrega `timestamp_us`, capturado
com `esp_timer_get_time()` no instante em que o dado foi copiado (logo após
o `recv()`, antes de entrar na fila). Cada handler calcula
`esp_timer_get_time() - evt->timestamp_us` e passa como `latency_us` pra
`process_data_from_tcp_client`/`process_data_from_remote_host`, que hoje só
logam esse valor. Isso mede o tempo entre "dado chegou na rede" e "task de
processamento pegou o dado da fila" — ou seja, inclui tempo de fila +
tempo de espera de scheduling, não só o processamento em si.

**Onde implementar o processamento de verdade**: edite
`process_data_from_tcp_client()` e `process_data_from_remote_host()` em
`main/app_main.c`. Recebem `(const uint8_t *data, size_t len, int64_t latency_us)`.
O buffer (`data`) é liberado pelo handler logo depois de chamar essas
funções — se for guardar o dado além do escopo da chamada, copie.

**Se a fila encher** (`proxy_events_post_data` não conseguir postar dentro
de `PROXY_EVENT_POST_TIMEOUT_MS`, hoje 100ms): o dado é descartado e um
warning é logado — a task de rede não fica bloqueada esperando
indefinidamente. Ajuste `PROXY_EVENT_QUEUE_SIZE`/`PROXY_EVENT_POST_TIMEOUT_MS`
em `components/proxy_events/proxy_events.c` conforme a rajada esperada.

**Limitações atuais a considerar**:
- Assim como o servidor, o proxy assume 1 conexão por vez em cada ponta.
- Se chegar dado do host remoto **antes** de haver um cliente TCP local
  conectado, `tcp_server_app_send()` (chamado dentro do handler) retorna
  `false` e loga um warning — o dado não é reenfileirado. Mesmo
  comportamento no sentido contrário.
- O `tcp_client_app` resolve o host via `getaddrinfo`, então tanto IP
  quanto hostname (se você tiver DNS configurado) funcionam.

## Segurança do MQTT: mTLS + ACL

O `mqtt_app` agora conecta via `mqtts://` com **TLS mútuo**: o ESP32 valida
o certificado do broker (pinning via CA própria) e o broker exige um
certificado válido do ESP32 pra aceitar a conexão. Isso resolve o cenário
que discutimos: mesmo que o cabo direto PC↔ESP32 vire um link através de
switch, alguém monitorando o tráfego não consegue nem ler o conteúdo
(TLS) nem se passar pelo ESP32 (o certificado do device não pode ser
forjado sem a chave privada correspondente).

### Passo a passo

1. **Gerar os certificados** (no seu PC, não em CI/ambiente compartilhado —
   as chaves privadas geradas são segredo):
   ```
   BROKER_IP=192.168.1.100 CLIENT_CN=esp32-device-01 ./scripts/generate_certs.sh
   ```
   Isso cria uma CA própria e assina um certificado pro broker (com o IP
   dele no SAN) e um pro ESP32, tudo em `out/certs/`.

2. **Copiar pro firmware**: substitua os placeholders em
   `components/mqtt_app/certs/` pelos arquivos gerados:
   ```
   cp out/certs/ca.crt out/certs/client.crt out/certs/client.key \
      components/mqtt_app/certs/
   ```
   Esses arquivos são embutidos no binário via `EMBED_TXTFILES` (ver
   `components/mqtt_app/CMakeLists.txt`) — não ficam num arquivo separado
   na flash, viram parte do `.bin` que você grava no ESP32.

3. **Configurar o Mosquitto no PC**: copie `out/certs/ca.crt`,
   `out/certs/broker.crt` e `out/certs/broker.key` pra onde o Mosquitto vai
   ler (ex: `/etc/mosquitto/certs/`), e use `mosquitto/mosquitto.conf.example`
   como base — ele liga `require_certificate true` (é o que torna isso
   mTLS e não só TLS) e `use_identity_as_username true` (usa o CN do
   certificado do ESP32 como identidade pro ACL).

4. **Configurar o ACL**: copie `mosquitto/acl.example` pra
   `/etc/mosquitto/acl.conf` (caminho referenciado no passo 3). Ele
   restringe o device (`esp32-device-01`) a só publicar em
   `device/tcp_server/client_connected` — mesmo que o certificado do
   device seja comprometido, o estrago fica limitado a esse tópico.

5. **Ajustar `CONFIG_MQTT_BROKER_URI`**: no menuconfig, mude pra
   `mqtts://<ip-do-pc>:8883` (esquema `mqtts://`, porta TLS do Mosquitto).

### O que NÃO está automatizado aqui

- **Provisionamento por device**: hoje um único `client.crt`/`client.key`
  é embutido no binário — se você tiver mais de um ESP32 no futuro, todos
  compilariam com a mesma identidade. Pra uma frota, o próximo passo seria
  gerar um certificado por device (`CLIENT_CN` diferente por unidade) e
  gravar via NVS/provisionamento individual em vez de embutir no `.bin`
  compilado uma vez pra todos.
- **Rotação de certificado**: os certificados gerados têm validade de 10
  anos (`DAYS_VALID` no script) — adequado pra um par fixo sem
  infraestrutura de renovação automática, mas se o certificado do ESP32
  precisar ser revogado antes disso (ex: device comprometido), a forma
  mais simples aqui é remover a permissão dele no ACL e/ou reemitir com
  outro `CLIENT_CN`, já que Mosquitto standalone não tem CRL/OCSP por
  padrão.

## Por que essa divisão

- **eth_w5500**: recebe uma `eth_w5500_config_param_t` (uma struct em vez de 13
  parâmetros posicionais) e devolve um `esp_netif_t*` já com IP estático
  configurado e handlers de evento registrados **por instância** — os logs
  agora dizem `[eth1]` ou `[eth2]`, o que faltava no código original.
- **mqtt_app**: isolado porque sua única dependência real é "em qual netif
  eu inicio a conexão". Não precisa saber nada sobre W5500 ou SPI.
- **tcp_server_app**: idem, só depende de uma porta TCP.
- **main**: fica praticamente declarativo — monta as duas configs e chama
  os três `_start()`/`_init()`. Fica fácil adicionar uma terceira interface
  ou trocar MQTT por outra coisa sem mexer no driver Ethernet.

## Correções aplicadas na reorganização

1. **MAC address**: `eth_w5500_init` agora chama `esp_read_mac(ESP_MAC_ETH)`
   e aplica `mac_addr_offset` (0 para eth1, 1 para eth2) antes de
   `esp_eth_start()`. No código original as duas placas subiam com o mesmo
   MAC placeholder do driver.
2. **Logs por interface**: os event handlers de Ethernet e IP agora recebem
   o `if_desc` como argumento, então dá pra saber no log qual das duas
   portas subiu/caiu.
3. **Race condition no MQTT**: o `esp_netif_set_default_netif()` de volta ao
   valor anterior só acontece no evento `MQTT_EVENT_CONNECTED`, não logo
   após `esp_mqtt_client_start()` (que é assíncrono).
4. **Clock SPI**: subido de 1 MHz para 20 MHz no `main` (ajuste conforme seu
   layout de trilhas — comentário deixado no código).
5. Pequenos ajustes: `IPPROTO_TCP` explícito, checagem de retorno de
   `esp_netif_new`/`esp_eth_new_netif_glue`, `app_main` para se `eth_w5500_init`
   falhar em vez de seguir com netif NULL.

## O que falta pra você preencher

- O `Kconfig.projbuild` com `CONFIG_ETH1_*`, `CONFIG_ETH2_*`,
  `CONFIG_MQTT_BROKER_URI` e `CONFIG_TCP_SERVER_PORT` — presumo que já
  existe no seu projeto original e não foi alterado aqui.
- `idf_component.yml` de cada componente, se você for publicá-los
  separadamente; para uso interno ao projeto não é necessário.

## Se quiser evoluir depois

- `tcp_server_app`: hoje aceita 1 cliente por vez (backlog=1). Pra múltiplos
  clientes simultâneos, trocar por `select()`/`poll()` ou task por conexão.
- `mqtt_app`: hoje só loga eventos de conexão/erro; dá pra expor um
  callback de mensagem recebida (`esp_mqtt_client_register_event` já
  aceita mais de um handler).
