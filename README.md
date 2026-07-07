# Estrutura reorganizada

```
components/
  eth_w5500/          # driver genérico de uma porta W5500 (SPI + MAC/PHY + netif)
  mqtt_app/            # cliente MQTT amarrado a um netif específico
  tcp_server_app/      # servidor TCP simples (1 conexão por vez)
  tcp_client_app/      # cliente TCP que conecta a um host remoto, com reconexão automática
  proxy_events/        # event loop dedicado pro processamento (desacoplado do I/O de rede)
main/
  app_main.c           # só orquestra: chama os 5 componentes acima e faz o proxy TCP
```

## Proxy TCP (tcp_server_app <-> tcp_client_app) via event loop dedicado

O `tcp_client_app` conecta como cliente TCP a `REMOTE_HOST_IP:REMOTE_HOST_PORT`
(hoje `192.168.1.100:50000`, definido em `main/app_main.c` — **ajuste pro IP
real**), reconectando automaticamente a cada 3s se a conexão cair.

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
