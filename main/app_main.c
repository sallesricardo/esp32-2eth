#include "sdkconfig.h"
#include <stdint.h>
#include <stdlib.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "eth_w5500.h"
#include "mqtt_app.h"
#include "tcp_server_app.h"
#include "tcp_client_app.h"
#include "proxy_events.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "app_main";

// Símbolos gerados pelo EMBED_TXTFILES do componente mqtt_app (ver
// components/mqtt_app/CMakeLists.txt). O linker cria esses nomes a partir
// do caminho do arquivo, com _binary_, o nome do arquivo com "." -> "_", e
// _start/_end.
extern const uint8_t mqtt_ca_cert_pem_start[]     asm("_binary_ca_crt_start");
extern const uint8_t mqtt_client_cert_pem_start[] asm("_binary_client_crt_start");
extern const uint8_t mqtt_client_key_pem_start[]  asm("_binary_client_key_start");

#define TCP_WELCOME_MSG      "Conectado ao servidor TCP\n"
#define MQTT_TCP_NOTIFY_TOPIC "device/tcp_server/client_connected"

#define REMOTE_HOST_IP   CONFIG_REMOTE_HOST_IP
#define REMOTE_HOST_PORT CONFIG_REMOTE_HOST_PORT

// Chamado pelo tcp_server_app assim que aceita uma nova conexão.
// Não conhece nada de MQTT diretamente sobre TCP -> quem faz a ponte é o main.
static void on_tcp_client_connected(const char *client_ip)
{
    ESP_LOGI(TAG, "Cliente TCP conectado: %s -> publicando no MQTT", client_ip);

    char payload[96];
    snprintf(payload, sizeof(payload), "{\"event\":\"tcp_client_connected\",\"ip\":\"%s\"}", client_ip);

    mqtt_app_publish(MQTT_TCP_NOTIFY_TOPIC, payload, /*qos=*/1, /*retain=*/0);
}

// --- Funções de processamento dos dados do proxy (só logging por enquanto) ---
//
// Rodam na task do event loop dedicado (proxy_events), NÃO na task de I/O
// de rede que recebeu o dado — então podem ficar mais pesadas no futuro
// sem travar a recepção do socket.

// Dados vindos do cliente TCP local (conectado no tcp_server_app), a
// caminho do host remoto.
static void process_data_from_tcp_client(const uint8_t *data, size_t len, int64_t latency_us)
{
    (void)data;
    ESP_LOGI(TAG, "process_data_from_tcp_client: %d bytes, latencia recv->processamento: %lld us",
             (int)len, (long long)latency_us);
    // TODO: implementar processamento
}

// Dados vindos do host remoto (via tcp_client_app), a caminho do cliente
// TCP local.
static void process_data_from_remote_host(const uint8_t *data, size_t len, int64_t latency_us)
{
    (void)data;
    ESP_LOGI(TAG, "process_data_from_remote_host: %d bytes, latencia recv->processamento: %lld us",
             (int)len, (long long)latency_us);
    // TODO: implementar processamento
}

// Handler do PROXY_EVENT_DATA_FROM_TCP_CLIENT, chamado pelo event loop
// dedicado. Processa e, na sequência, faz a ponta do proxy nesse sentido
// (encaminha pro host remoto). Dono do evt->data: libera no final.
static void handle_data_from_tcp_client_event(void *handler_arg, esp_event_base_t base,
                                               int32_t event_id, void *event_data)
{
    (void)handler_arg;
    (void)base;
    (void)event_id;
    proxy_data_event_t *evt = (proxy_data_event_t *)event_data;
    int64_t latency_us = esp_timer_get_time() - evt->timestamp_us;

    process_data_from_tcp_client(evt->data, evt->len, latency_us);
    tcp_client_app_send(evt->data, evt->len); // proxy: repassa pro host remoto

    free(evt->data);
}

// Handler do PROXY_EVENT_DATA_FROM_REMOTE_HOST, chamado pelo event loop
// dedicado. Processa e, na sequência, faz a ponta do proxy nesse sentido
// (encaminha pro cliente TCP local). Dono do evt->data: libera no final.
static void handle_data_from_remote_host_event(void *handler_arg, esp_event_base_t base,
                                                int32_t event_id, void *event_data)
{
    (void)handler_arg;
    (void)base;
    (void)event_id;
    proxy_data_event_t *evt = (proxy_data_event_t *)event_data;
    int64_t latency_us = esp_timer_get_time() - evt->timestamp_us;

    process_data_from_remote_host(evt->data, evt->len, latency_us);
    tcp_server_app_send(evt->data, evt->len); // proxy: repassa pro cliente TCP local

    free(evt->data);
}

// Callback chamado pelo tcp_server_app na própria task de I/O de rede, a
// cada dado recebido do cliente TCP local. Fica leve de propósito: só
// copia + timestampa + enfileira no event loop dedicado, sem processar
// aqui (isso é feito em handle_data_from_tcp_client_event).
static void on_data_from_tcp_client(const uint8_t *data, size_t len)
{
    proxy_events_post_data(PROXY_EVENT_DATA_FROM_TCP_CLIENT, data, len);
}

// Idem, mas pro lado do tcp_client_app (dado vindo do host remoto).
static void on_data_from_remote_host(const uint8_t *data, size_t len)
{
    proxy_events_post_data(PROXY_EVENT_DATA_FROM_REMOTE_HOST, data, len);
}

void app_main(void)
{
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // --- W5500 #1: usado para MQTT ---
    eth_w5500_config_param_t eth1_cfg = {
        .spi_host        = SPI2_HOST,
        .miso_gpio       = CONFIG_ETH1_W5500_SPI_MISO,
        .mosi_gpio       = CONFIG_ETH1_W5500_SPI_MOSI,
        .sclk_gpio       = CONFIG_ETH1_W5500_SPI_SCLK,
        .cs_gpio         = CONFIG_ETH1_W5500_SPI_CS,
        .int_gpio        = CONFIG_ETH1_W5500_INT,
        .rst_gpio        = CONFIG_ETH1_W5500_RST,
        .clock_speed_hz  = 40 * 1000 * 1000, // ajuste conforme layout/trilhas
        .ip_addr         = CONFIG_ETH1_STATIC_IP_ADDR,
        .gw_addr         = CONFIG_ETH1_STATIC_GW_ADDR,
        .netmask_addr    = CONFIG_ETH1_STATIC_NETMASK_ADDR,
        .dns_addr        = CONFIG_ETH1_STATIC_DNS_ADDR,
        .if_key          = "ETH1",
        .if_desc         = "eth1",
        .route_prio      = 100,
        .mac_addr_offset = 0,
    };

    // --- W5500 #2: usado para o servidor TCP ---
    eth_w5500_config_param_t eth2_cfg = {
        .spi_host        = SPI3_HOST,
        .miso_gpio       = CONFIG_ETH2_W5500_SPI_MISO,
        .mosi_gpio       = CONFIG_ETH2_W5500_SPI_MOSI,
        .sclk_gpio       = CONFIG_ETH2_W5500_SPI_SCLK,
        .cs_gpio         = CONFIG_ETH2_W5500_SPI_CS,
        .int_gpio        = CONFIG_ETH2_W5500_INT,
        .rst_gpio        = CONFIG_ETH2_W5500_RST,
        .clock_speed_hz  = 40 * 1000 * 1000,
        .ip_addr         = CONFIG_ETH2_STATIC_IP_ADDR,
        .gw_addr         = CONFIG_ETH2_STATIC_GW_ADDR,
        .netmask_addr    = CONFIG_ETH2_STATIC_NETMASK_ADDR,
        .dns_addr        = CONFIG_ETH2_STATIC_DNS_ADDR,
        .if_key          = "ETH2",
        .if_desc         = "eth2",
        .route_prio      = 90,
        .mac_addr_offset = 1, // diferente de eth1_cfg -> MAC distinto
    };

    ESP_LOGI(TAG, "Initializing W5500 #1 (MQTT)...");
    esp_netif_t *eth_netif_1 = eth_w5500_init(&eth1_cfg);

    ESP_LOGI(TAG, "Initializing W5500 #2 (TCP Server)...");
    esp_netif_t *eth_netif_2 = eth_w5500_init(&eth2_cfg);

    if (eth_netif_1 == NULL || eth_netif_2 == NULL) {
        ESP_LOGE(TAG, "Falha ao inicializar uma das interfaces Ethernet, abortando.");
        return;
    }

    mqtt_app_config_t mqtt_cfg = {
        .eth_netif = eth_netif_1,
        .ca_cert_pem = (const char *)mqtt_ca_cert_pem_start,
        .client_cert_pem = (const char *)mqtt_client_cert_pem_start,
        .client_key_pem = (const char *)mqtt_client_key_pem_start,
    };
    mqtt_app_start(&mqtt_cfg);

    // Array de interfaces na mesma ordem referenciada por
    // CONFIG_TCP_PROXY_BIND_ETH_IDX (0 = ETH1, 1 = ETH2). Se adicionar mais
    // W5500 no futuro, é só estender esse array e o range no Kconfig.projbuild.
    esp_netif_t *eth_netifs[] = { eth_netif_1, eth_netif_2 };
    const int proxy_bind_idx = CONFIG_TCP_PROXY_BIND_ETH_IDX;

    esp_netif_t *proxy_bind_netif = NULL;
    if (proxy_bind_idx >= 0 && proxy_bind_idx < (int)(sizeof(eth_netifs) / sizeof(eth_netifs[0]))) {
        proxy_bind_netif = eth_netifs[proxy_bind_idx];
        ESP_LOGI(TAG, "Proxy TCP fixado na interface: %s (indice %d)",
                 esp_netif_get_desc(proxy_bind_netif), proxy_bind_idx);
    } else if (proxy_bind_idx == -1) {
        ESP_LOGI(TAG, "Proxy TCP: bind automatico (tabela de rotas do lwIP decide)");
    } else {
        ESP_LOGW(TAG, "CONFIG_TCP_PROXY_BIND_ETH_IDX=%d fora do range esperado, "
                       "usando automatico (tabela de rotas)", proxy_bind_idx);
    }

    // Event loop dedicado do proxy: task própria, fila própria, separada
    // do loop default (ETH_EVENT/IP_EVENT). O processamento roda aqui,
    // desacoplado das tasks de I/O de rede do tcp_server_app/tcp_client_app.
    ESP_ERROR_CHECK(proxy_events_init());
    ESP_ERROR_CHECK(proxy_events_register_handler(
        PROXY_EVENT_DATA_FROM_TCP_CLIENT, handle_data_from_tcp_client_event, NULL));
    ESP_ERROR_CHECK(proxy_events_register_handler(
        PROXY_EVENT_DATA_FROM_REMOTE_HOST, handle_data_from_remote_host_event, NULL));

    tcp_server_app_start(CONFIG_TCP_SERVER_PORT, proxy_bind_netif, TCP_WELCOME_MSG,
                          on_tcp_client_connected, on_data_from_tcp_client);
    tcp_client_app_start(REMOTE_HOST_IP, REMOTE_HOST_PORT, proxy_bind_netif, on_data_from_remote_host);
}
