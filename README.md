# Estrutura reorganizada

```
components/
  eth_w5500/          # driver genérico de uma porta W5500 (SPI + MAC/PHY + netif)
  mqtt_app/            # cliente MQTT com mTLS, amarrado a um netif específico
    certs/              # PLACEHOLDERS -- substitua pelos gerados em scripts/generate_certs.sh
  tcp_server_app/      # servidor TCP simples (1 conexão por vez)
  tcp_client_app/      # cliente TCP que conecta a um host remoto, com reconexão automática
  proxy_events/        # event loop dedicado pro processamento (desacoplado do I/O de rede)
  device_config/       # config recebida via MQTT, salva no NVS com hash-gate (CRC32)
main/
  app_main.c           # orquestra: chama os componentes acima e faz o proxy TCP
                        #   (ou o backend Wi-Fi de teste, ver seção abaixo)
  Kconfig.projbuild    # backend de rede (W5500 x Wi-Fi de teste), GPIOs dos
                        #   W5500, broker MQTT, porta TCP, bind do proxy
scripts/
  generate_certs.sh        # gera/reaproveita a CA própria + certs do broker e do ESP32
  generate_client_cert.sh  # emite um certificado de cliente A MAIS, reaproveitando a CA
mosquitto/
  mosquitto.conf.example  # config de exemplo do broker (mTLS)
  acl.example              # ACL de exemplo restringindo tópicos do device
```

## Backend de rede: W5500 (produção) x Wi-Fi (teste de bancada)

O firmware roda de fábrica com os dois W5500 (ESP32-S3), mas dá pra testar
a lógica de aplicação numa ESP32 qualquer sem os módulos W5500 disponíveis,
via `idf.py menuconfig` → "Configuração Dual Ethernet W5500" → "Backend de
rede":

- **Dual W5500 (produção)** — default. Comportamento original: dois netifs
  W5500 (`eth_netif_1`/`eth_netif_2`), um pro MQTT e outro pro proxy TCP,
  cada um com seu IP estático (`CONFIG_ETH1_*`/`CONFIG_ETH2_*`).
- **Wi-Fi STA (teste de bancada sem W5500)** — sobe **uma** interface Wi-Fi
  (`CONFIG_TEST_WIFI_SSID`/`CONFIG_TEST_WIFI_PASSWORD`, opções que só
  aparecem com esse backend selecionado) e a usa tanto como `eth_netif_1`
  quanto `eth_netif_2`. Como é a mesma interface nos dois papéis, isso
  **não** valida a separação real de tráfego entre duas NICs físicas — mas
  exercita 100% do resto: conexão MQTT com mTLS, `tcp_server_app`,
  `tcp_client_app`, o proxy via `proxy_events`, etc.

Nenhum código dos componentes (`mqtt_app`, `tcp_server_app`,
`tcp_client_app`, `proxy_events`) muda entre os dois backends — só
`main/app_main.c` decide, em tempo de compilação
(`#if CONFIG_NETIF_BACKEND_WIFI`), qual interface inicializar. Trocar de
volta pra produção é só escolher "Dual W5500" de novo no menuconfig; os
valores de `CONFIG_ETH1_*`/`CONFIG_ETH2_*` continuam salvos no `sdkconfig`.

`esp_wifi`/`nvs_flash` estão sempre no `REQUIRES` de `main/CMakeLists.txt`
(o ESP-IDF não permite condicionar `REQUIRES` a valores `CONFIG_*` — a
expansão de dependências roda antes do sdkconfig ser carregado). No
firmware de produção (backend W5500) esse código nunca é chamado — fica
atrás de `#if CONFIG_NETIF_BACKEND_WIFI` em `app_main.c` — e o linker
(`--gc-sections`, padrão no IDF) descarta os objetos não referenciados do
binário final.

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

**`main/Kconfig.projbuild`**: já vem completo neste repo, com o `choice`
de backend de rede, todas as opções `CONFIG_ETH1_*`/`CONFIG_ETH2_*` (GPIOs
e IP estático de cada W5500), `CONFIG_MQTT_BROKER_URI`,
`CONFIG_TCP_SERVER_PORT` e `CONFIG_TCP_PROXY_BIND_ETH_IDX`. Não precisa
mesclar com nada.

**SPI host configurável**: `CONFIG_ETH1_W5500_SPI_HOST`/
`CONFIG_ETH2_W5500_SPI_HOST` (1=SPI2_HOST/HSPI, 2=SPI3_HOST/VSPI) são lidos
direto em `main/app_main.c` — se precisar inverter as portas físicas dos
dois W5500, é só mudar no menuconfig, sem recompilar o mapeamento. Se por
engano os dois ficarem no mesmo host, `app_main` detecta e loga um erro
claro antes de chamar `eth_w5500_init` (em vez de um `ESP_ERROR_CHECK`
genérico no meio do driver).

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

   Se o broker for acessível por mais de um IP (ex: PC com mais de uma
   interface de rede, ou você ainda não decidiu qual IP vai usar em
   produção), use `BROKER_IPS` (separado por vírgula) em vez de
   `BROKER_IP` — o certificado do broker sai com todos no SAN:
   ```
   BROKER_IPS="192.168.1.100,192.168.2.1" CLIENT_CN=esp32-device-01 ./scripts/generate_certs.sh
   ```
   Todo IP que algum dia for usado em `CONFIG_MQTT_BROKER_URI` precisa
   estar nessa lista — o mbedTLS (no ESP32) valida o IP da URI contra o
   SAN do certificado do broker, e derruba a conexão TLS se não achar uma
   correspondência (não adianta só colocar o IP no `CN`; SAN é o que
   importa nas stacks TLS modernas).

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
   `device/tcp_server/client_connected` e `device/tcp_client/data_hex` —
   mesmo que o certificado do device seja comprometido, o estrago fica
   limitado a esses dois tópicos.

5. **Ajustar `CONFIG_MQTT_BROKER_URI`**: no menuconfig, mude pra
   `mqtts://<ip-do-pc>:8883` (esquema `mqtts://`, porta TLS do Mosquitto).

6. **Adicionar outros clientes mTLS depois** (ex: uma app rodando no mesmo
   PC do broker, outro ESP32, uma ferramenta de debug): **não rode
   `generate_certs.sh` de novo pra isso** — ele recria a CA do zero e
   invalida o `ca.crt` já embutido no firmware e os certificados já
   emitidos. Use `scripts/generate_client_cert.sh`, que reaproveita a CA
   existente:
   ```
   CLIENT_CN=pc-app-01 ./scripts/generate_client_cert.sh
   ```
   Isso gera só `pc-app-01.crt`/`pc-app-01.key` (assinados pela mesma CA),
   sem mexer no `broker.crt` nem no `ca.crt`. Depois é só:
   - copiar `ca.crt` + `pc-app-01.crt` + `pc-app-01.key` pra onde a app vai
     ler os certificados;
   - adicionar `user pc-app-01` + os tópicos dela em `mosquitto/acl.conf`
     (tem um exemplo comentado em `mosquitto/acl.example`);
   - `systemctl restart mosquitto` (ele não recarrega ACL/certs sozinho).

   Se essa nova app for conectar via `127.0.0.1` (ou outro IP), o
   certificado **do broker** também precisa desse IP no SAN — reveja o
   passo 1 (`BROKER_IPS`/`BROKER_DNS_NAMES`). Rodar `generate_certs.sh` de
   novo só pra atualizar o SAN do broker é seguro: ele detecta que a CA já
   existe e reaproveita (não regenera), só reemite o `broker.crt`.

### O que NÃO está automatizado aqui

- **Provisionamento por device em escala**: `scripts/generate_client_cert.sh`
  já cobre emitir um certificado a mais (outra app, outro device) sem
  invalidar a CA. O que ainda não existe é automação pra frota — se você
  tiver muitos ESP32, hoje cada um precisaria de um `CLIENT_CN` próprio
  gerado manualmente e embutido no `.bin` compilado individualmente; o
  próximo passo seria gravar o cert/key via NVS/provisionamento por device
  em vez de embutir no binário.
- **Rotação de certificado**: os certificados gerados têm validade de 10
  anos (`DAYS_VALID` no script) — adequado pra um par fixo sem
  infraestrutura de renovação automática, mas se o certificado do ESP32
  precisar ser revogado antes disso (ex: device comprometido), a forma
  mais simples aqui é remover a permissão dele no ACL e/ou reemitir com
  outro `CLIENT_CN`, já que Mosquitto standalone não tem CRL/OCSP por
  padrão.

## Payload MQTT: cJSON em vez de snprintf

`on_tcp_client_connected` e `process_data_from_remote_host` (em
`main/app_main.c`) montam o payload JSON com `cJSON` (componente `json`,
já embutido no ESP-IDF — sem dependência externa) em vez de `snprintf`
manual. Motivo: qualquer valor interpolado (como `client_ip`) recebe
escaping automático, o que importa porque esse dado passa por rede não
totalmente confiável no modelo de ameaça deste projeto. Também fica mais
fácil adicionar campos novos sem recalcular tamanho de buffer manualmente.

Padrão usado:
```c
cJSON *root = cJSON_CreateObject();
cJSON_AddStringToObject(root, "event", "tcp_client_connected");
cJSON_AddStringToObject(root, "ip", client_ip);
char *payload = cJSON_PrintUnformatted(root);
cJSON_Delete(root);
mqtt_app_publish(MQTT_TCP_NOTIFY_TOPIC, payload, 1, 0);
cJSON_free(payload); // cJSON_Print* aloca via cJSON_malloc -> libera com cJSON_free, não free()
```

**Tópicos publicados hoje:**

| Tópico | Quando | Payload |
|---|---|---|
| `device/tcp_server/client_connected` | um cliente conecta no `tcp_server_app` (porta local) | `{"event":"tcp_client_connected","ip":"..."}` |
| `device/tcp_client/data_hex` | o `tcp_client_app` recebe dado do host remoto | `{"event":"tcp_client_data","hex":"...","len":N}` |

O dado recebido do lado do `tcp_server_app` (`process_data_from_tcp_client`)
ainda só loga — não é publicado no MQTT. Se quiser o mesmo tratamento
nesse sentido, é só espelhar o padrão de `process_data_from_remote_host`
com outro tópico/campo `event`.

**Sobre o campo `hex`**: cada byte recebido vira 2 caracteres hex
minúsculos (`bytes_to_hex` em `main/app_main.c`), então o payload MQTT
fica ~2x o tamanho do dado bruto — aceitável pro volume desse proxy, mas
vale lembrar se o tráfego TCP crescer bastante.

## Config recebida via MQTT, salva no NVS (device_config)

O firmware assina `device/config/set` (ver `MQTT_CONFIG_TOPIC` em
`main/app_main.c`) e, a cada mensagem recebida, delega pro componente
**`device_config`** (`components/device_config/`) — genérico, não sabe
nada de MQTT, só entende "bytes crus + hash + NVS".

**Fluxo hoje:**

1. `mqtt_app` assina `device/config/set` automaticamente (a cada conexão
   e reconexão — ver "Subscriptions" abaixo) e reagrupa a mensagem
   completa antes de repassar (mensagens MQTT maiores que o buffer
   interno do client chegam fragmentadas em vários `MQTT_EVENT_DATA`;
   `mqtt_app.c` já cuida disso).
2. `on_mqtt_config_data` (`main/app_main.c`) recebe o payload bruto e
   chama `device_config_apply_if_changed()`.
3. `device_config` calcula um **CRC32** do payload novo e compara com o
   hash do último config salvo no NVS:
   - **Igual** → não escreve nada na flash, só loga. Isso importa porque
     é comum o mesmo config chegar de novo sem ter mudado — mensagem
     retida (`retain=1`) sendo entregue a cada reconexão MQTT, ou o
     publicador simplesmente republicando o mesmo valor. Sem esse
     hash-gate, cada uma dessas entregas viraria uma escrita na flash à
     toa (a NVS tem wear-leveling, mas não é motivo pra desperdiçar
     ciclos de escrita de propósito).
   - **Diferente** → salva o payload bruto + o hash novo no NVS
     (namespace `dev_cfg`, chaves `cfg_raw`/`cfg_hash`).
4. No próximo boot, `device_config_load_raw()` recupera o último config
   salvo — o device não fica "sem config" só porque ainda não reconectou
   no MQTT depois de um reboot.

**O que falta (de propósito — você ainda não definiu o schema):** hoje
`device_config` só guarda o payload bruto; ele não sabe interpretar
campos específicos. Quando o schema estiver definido, o parse/aplicação
entra no `TODO` já deixado dentro de `on_mqtt_config_data`
(`main/app_main.c`) — provavelmente `cJSON_ParseWithLength()` no payload
reagrupado, seguindo o mesmo padrão de uso de cJSON já estabelecido nesse
projeto (ver seção "Payload MQTT" acima).

**Por que CRC32 e não um hash criptográfico (SHA-256 etc.)**: aqui o hash
só serve pra detectar *mudança de conteúdo*, não pra garantir
integridade/autenticidade — isso já é responsabilidade do canal mTLS (a
mensagem só chega até aqui se a conexão MQTT foi autenticada nos dois
sentidos). Um hash criptográfico custaria mais CPU sem ganhar nada nesse
caso de uso. Se um dia esse fluxo aceitar config de uma fonte não
autenticada, essa decisão precisa ser revisitada.

**Subscriptions no mqtt_app**: o `mqtt_app` ganhou suporte a assinar
tópicos automaticamente (`mqtt_app_config_t.subscriptions`) e a um
callback de dados recebidos (`on_data`) — reassina em toda
`MQTT_EVENT_CONNECTED` porque reconexão não preserva subscriptions a
menos que o broker use sessão persistente. Adicionar um tópico novo (além
de `device/config/set`) é só estender o array `mqtt_subscriptions` em
`main/app_main.c`.

**Lembrete de ACL**: `esp32-device-01` precisa de `topic read
device/config/set` no `acl.conf` do Mosquitto pra conseguir assinar esse
tópico — já está em `mosquitto/acl.example`, mas se você já tem um
`acl.conf` em produção, precisa adicionar a linha lá também e reiniciar o
Mosquitto.

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
4. **Clock SPI**: subido de 1 MHz para 40 MHz no `main` (ajuste conforme seu
   layout de trilhas — comentário deixado no código; em fiação longa/jumper
   considere baixar pra ~20 MHz se notar erros de comunicação com o W5500).
5. Pequenos ajustes: `IPPROTO_TCP` explícito, checagem de retorno de
   `esp_netif_new`/`esp_eth_new_netif_glue`, `app_main` para se `eth_w5500_init`
   falhar em vez de seguir com netif NULL.

## O que falta pra você preencher

- `idf_component.yml` de cada componente, se você for publicá-los
  separadamente; para uso interno ao projeto não é necessário.
- Substituir os placeholders em `components/mqtt_app/certs/` pelos
  certificados reais (ver seção "Segurança do MQTT" acima) antes de gravar
  em produção.

## Se quiser evoluir depois

- `tcp_server_app`: hoje aceita 1 cliente por vez (backlog=1). Pra múltiplos
  clientes simultâneos, trocar por `select()`/`poll()` ou task por conexão.
- `mqtt_app`: hoje só loga eventos de conexão/erro; dá pra expor um
  callback de mensagem recebida (`esp_mqtt_client_register_event` já
  aceita mais de um handler).
- Backend de teste Wi-Fi: hoje só existe modo STA fixo (SSID/senha via
  Kconfig). Se for usar bastante, dá pra evoluir pra provisionamento via
  `wifi_provisioning` em vez de hardcodar credenciais no `sdkconfig`.
