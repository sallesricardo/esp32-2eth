# Estrutura reorganizada

```
components/
  eth_w5500/          # driver genérico de uma porta W5500 (SPI + MAC/PHY + netif)
  mqtt_app/            # cliente MQTT amarrado a um netif específico
  tcp_server_app/      # servidor TCP simples (1 conexão por vez)
main/
  app_main.c           # só orquestra: chama os 3 componentes acima
```

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
