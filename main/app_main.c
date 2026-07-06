#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "eth_w5500.h"
#include "mqtt_app.h"
#include "tcp_server_app.h"

static const char *TAG = "app_main";

#define TCP_WELCOME_MSG      "Conectado ao servidor TCP\n"
#define MQTT_TCP_NOTIFY_TOPIC "device/tcp_server/client_connected"

// Chamado pelo tcp_server_app assim que aceita uma nova conexão.
// Não conhece nada de MQTT diretamente sobre TCP -> quem faz a ponte é o main.
static void on_tcp_client_connected(const char *client_ip)
{
    ESP_LOGI(TAG, "Cliente TCP conectado: %s -> publicando no MQTT", client_ip);

    char payload[96];
    snprintf(payload, sizeof(payload), "{\"event\":\"tcp_client_connected\",\"ip\":\"%s\"}", client_ip);

    mqtt_app_publish(MQTT_TCP_NOTIFY_TOPIC, payload, /*qos=*/1, /*retain=*/0);
}

static void on_tcp_client_received(size_t len, const char *data)
{
    ESP_LOGI(TAG, "Cliente TCP Recebeu dados -> publicando no MQTT");

    char payload[96];
    snprintf(payload, sizeof(payload), "{\"event\":\"tcp_client_received\",\"data\":\"%.*s\"}", (int)len, data);

    mqtt_app_publish(MQTT_TCP_NOTIFY_TOPIC, payload, /*qos=*/1, /*retain=*/0);
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

    mqtt_app_start(eth_netif_1);
    tcp_server_app_start(CONFIG_TCP_SERVER_PORT, TCP_WELCOME_MSG, on_tcp_client_connected, on_tcp_client_received);

    (void)eth_netif_2; // usado indiretamente: TCP server escuta em INADDR_ANY
}
