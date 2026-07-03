#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include "mqtt_client.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_netif_sntp.h"
#include "esp_netif_types.h"
#include "esp_eth_driver.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_netif_defaults.h"
#include "esp_netif_types.h"
#include "esp_netif_net_stack.h"

static const char *TAG = "DUAL_ETH";

// Definições de GPIO para W5500_1 (MQTT)
#define ETH1_SPI_HOST       SPI2_HOST
#define ETH1_MISO_GPIO      CONFIG_ETH1_W5500_SPI_MISO
#define ETH1_MOSI_GPIO      CONFIG_ETH1_W5500_SPI_MOSI
#define ETH1_SCLK_GPIO      CONFIG_ETH1_W5500_SPI_SCLK
#define ETH1_CS_GPIO        CONFIG_ETH1_W5500_SPI_CS
#define ETH1_INT_GPIO       CONFIG_ETH1_W5500_INT
#define ETH1_RST_GPIO       CONFIG_ETH1_W5500_RST

// Definições de GPIO para W5500_2 (TCP Server)
#define ETH2_SPI_HOST       SPI3_HOST
#define ETH2_MISO_GPIO      CONFIG_ETH2_W5500_SPI_MISO
#define ETH2_MOSI_GPIO      CONFIG_ETH2_W5500_SPI_MOSI
#define ETH2_SCLK_GPIO      CONFIG_ETH2_W5500_SPI_SCLK
#define ETH2_CS_GPIO        CONFIG_ETH2_W5500_SPI_CS
#define ETH2_INT_GPIO       CONFIG_ETH2_W5500_INT
#define ETH2_RST_GPIO       CONFIG_ETH2_W5500_RST

typedef struct
{
    esp_eth_handle_t eth_handle;

    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;

    esp_netif_t *netif;

} ethernet_port_t;

// Event handler para Ethernet
static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = (esp_netif_t *)arg;

    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

// Event handler para IP
static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Addr");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETH IP: " IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETH MASK: " IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETH GW: " IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
}

// Função de inicialização do W5500
static esp_netif_t* eth_w5500_init(
    spi_host_device_t spi_host, 
    int miso_gpio,
    int mosi_gpio,
    int sclk_gpio,
    int cs_gpio,
    int int_gpio,
    int rst_gpio,
    const char* ip_addr_str,
    const char* gw_addr_str,
    const char* netmask_addr_str,
    const char* dns_addr_str,
    const char* if_key,
    const char* if_desc,
    int route_prio
)
{
    // Configuração SPI bus
    spi_bus_config_t buscfg = {
        .miso_io_num = miso_gpio,
        .mosi_io_num = mosi_gpio,
        .sclk_io_num = sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_LOGI(TAG, "Initializing SPI bus for W5500...");
    ESP_ERROR_CHECK(spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO));

    // Configuração W5500 Ethernet driver
    spi_device_interface_config_t spi_devcfg = {
        .command_bits = 16, // 0x00 for read, 0x80 for write
        .address_bits = 8,
        .dummy_bits = 0,
        .mode = 0,
        .clock_speed_hz = 1 * 1000 * 1000, // 2MHz
        .spics_io_num = cs_gpio,
        .queue_size = 20,
    };
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_host, &spi_devcfg);
    w5500_config.base.int_gpio_num = int_gpio;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = rst_gpio;

    ESP_LOGI(TAG, "Initializing W5500 MAC...");
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    ESP_LOGI(TAG, "Initializing W5500 PHY...");
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_LOGI(TAG, "Installing ETH driver...");
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    /* W5500 doesn't have SMI interface, so we don't need to call esp_eth_smi_init */
    /* W5500 doesn't have SMI interface, so we don't need to call esp_eth_smi_start */

    // Inicializa esp_netif com IP estático
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();

    base_cfg.if_key  = if_key;
    base_cfg.if_desc = if_desc;
    base_cfg.route_prio = route_prio;

    esp_netif_config_t cfg = {
        .base   = &base_cfg,
        .driver = NULL,
        .stack  = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    esp_netif_t *eth_netif = esp_netif_new(&cfg);

    assert(eth_netif);

    esp_netif_dns_info_t dns_info = {0};
    esp_netif_str_to_ip4(dns_addr_str, &dns_info.ip.u_addr.ip4);
    esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info);

    ESP_LOGI(TAG, "Attaching ETH driver to the netif...");
    void *glue = esp_eth_new_netif_glue(eth_handle);
    assert(glue);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    ESP_LOGI(TAG, "Stopping DHCP client on the interface...");
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_str_to_ip4(ip_addr_str, &ip_info.ip);
    esp_netif_str_to_ip4(netmask_addr_str, &ip_info.netmask);
    esp_netif_str_to_ip4(gw_addr_str, &ip_info.gw);
    esp_netif_set_ip_info(eth_netif, &ip_info);

    ESP_LOGI(TAG, "Starting ETH driver...");
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    uint8_t mac[6];

    ESP_ERROR_CHECK(
        esp_eth_ioctl(
            eth_handle,
            ETH_CMD_G_MAC_ADDR,
            mac));

    ESP_LOGI(TAG,
        "MAC = %02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2],
        mac[3], mac[4], mac[5]);

    return eth_netif;
}

// Função para o cliente MQTT
static void mqtt_app_start(esp_netif_t *eth_netif)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
    };

    esp_netif_t *default_netif = esp_netif_get_default_netif();
    esp_netif_set_default_netif(eth_netif);

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);

    if (default_netif) {
        esp_netif_set_default_netif(default_netif);
    }
}

// Função para o servidor TCP
static void tcp_server_task(void *pvParameters)
{
    char rx_buffer[128];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    struct sockaddr_in dest_addr;

    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(CONFIG_TCP_SERVER_PORT);
    ip_protocol = IPPROTO_IP;

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", CONFIG_TCP_SERVER_PORT);

    err = listen(listen_sock, 1); // Apenas uma conexão por vez para simplicidade
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket listening");

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Convert ip address to string
        if (source_addr.sin_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
        }
        ESP_LOGI(TAG, "Socket accepted ip: %s", addr_str);

        int len;
        do {
            len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0) {
                ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
            } else if (len == 0) {
                ESP_LOGW(TAG, "Connection closed");
            } else {
                rx_buffer[len] = 0; // Null-terminate whatever we received and print it
                ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);
                // Aqui você pode processar os dados binários recebidos
            }
        } while (len > 0);

        shutdown(sock, 0);
        close(sock);
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

void app_main(void)
{
    gpio_install_isr_service(0);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Registra os event handlers para ambas as interfaces
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // Inicializa a primeira interface Ethernet (MQTT)
    ESP_LOGI(TAG, "Initializing W5500 #1 (MQTT)...");
    esp_netif_t *eth_netif_1 = eth_w5500_init(
        ETH1_SPI_HOST,
        ETH1_MISO_GPIO,
        ETH1_MOSI_GPIO,
        ETH1_SCLK_GPIO,
        ETH1_CS_GPIO,
        ETH1_INT_GPIO,
        ETH1_RST_GPIO,
        CONFIG_ETH1_STATIC_IP_ADDR,
        CONFIG_ETH1_STATIC_GW_ADDR,
        CONFIG_ETH1_STATIC_NETMASK_ADDR,
        CONFIG_ETH1_STATIC_DNS_ADDR,
        "ETH1",
        "eth1",
        100
    );

    // Inicializa a segunda interface Ethernet (TCP Server)
    ESP_LOGI(TAG, "Initializing W5500 #2 (TCP Server)...");
    esp_netif_t *eth_netif_2 = eth_w5500_init(
        ETH2_SPI_HOST,
        ETH2_MISO_GPIO,
        ETH2_MOSI_GPIO,
        ETH2_SCLK_GPIO,
        ETH2_CS_GPIO,
        ETH2_INT_GPIO,
        ETH2_RST_GPIO,
        CONFIG_ETH2_STATIC_IP_ADDR,
        CONFIG_ETH2_STATIC_GW_ADDR,
        CONFIG_ETH2_STATIC_NETMASK_ADDR,
        CONFIG_ETH2_STATIC_DNS_ADDR,
        "ETH2",
        "eth2",
        90
    );

    // Inicia o cliente MQTT na primeira interface
    mqtt_app_start(eth_netif_1);

    // Inicia o servidor TCP na segunda interface
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
}
