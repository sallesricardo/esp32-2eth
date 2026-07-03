#include "eth_w5500.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_eth_driver.h"
#include "esp_netif_defaults.h"
#include "esp_netif_net_stack.h"

static const char *TAG = "eth_w5500";

// --- Event handlers (por instância, via arg = if_desc) ---

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    const char *if_desc = (const char *)arg;
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    uint8_t mac_addr[6] = {0};

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "[%s] Link Up, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 if_desc, mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[%s] Link Down", if_desc);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "[%s] Started", if_desc);
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "[%s] Stopped", if_desc);
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    (void)event_id;
    const char *if_desc = (const char *)arg;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "[%s] Got IP: " IPSTR "  MASK: " IPSTR "  GW: " IPSTR,
             if_desc, IP2STR(&ip_info->ip), IP2STR(&ip_info->netmask), IP2STR(&ip_info->gw));
}

esp_netif_t *eth_w5500_init(const eth_w5500_config_param_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "config NULL");
        return NULL;
    }

    // --- SPI bus ---
    spi_bus_config_t buscfg = {
        .miso_io_num = config->miso_gpio,
        .mosi_io_num = config->mosi_gpio,
        .sclk_io_num = config->sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_LOGI(TAG, "[%s] Initializing SPI bus...", config->if_desc);
    ESP_ERROR_CHECK(spi_bus_initialize(config->spi_host, &buscfg, SPI_DMA_CH_AUTO));

    // --- W5500 driver ---
    spi_device_interface_config_t spi_devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .dummy_bits = 0,
        .mode = 0,
        .clock_speed_hz = config->clock_speed_hz,
        .spics_io_num = config->cs_gpio,
        .queue_size = 20,
    };
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(config->spi_host, &spi_devcfg);
    w5500_config.base.int_gpio_num = config->int_gpio;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = config->rst_gpio;

    ESP_LOGI(TAG, "[%s] Initializing MAC/PHY...", config->if_desc);
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (mac == NULL || phy == NULL) {
        ESP_LOGE(TAG, "[%s] Falha ao criar MAC/PHY", config->if_desc);
        return NULL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    // --- MAC address: o W5500 não tem MAC de fábrica, então definimos
    // explicitamente a partir do MAC base do chip, com offset para
    // diferenciar as duas instâncias W5500. Sem isso as duas placas sobem
    // com o mesmo MAC "placeholder" do driver. ---
    uint8_t mac_addr[6];
    ESP_ERROR_CHECK(esp_read_mac(mac_addr, ESP_MAC_ETH));
    mac_addr[5] = (uint8_t)(mac_addr[5] + config->mac_addr_offset);
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    // --- esp_netif com IP estático ---
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.if_key = config->if_key;
    base_cfg.if_desc = config->if_desc;
    base_cfg.route_prio = config->route_prio;

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    if (eth_netif == NULL) {
        ESP_LOGE(TAG, "[%s] esp_netif_new falhou", config->if_desc);
        return NULL;
    }

    esp_netif_dns_info_t dns_info = {0};
    esp_netif_str_to_ip4(config->dns_addr, &dns_info.ip.u_addr.ip4);
    esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info);

    void *glue = esp_eth_new_netif_glue(eth_handle);
    if (glue == NULL) {
        ESP_LOGE(TAG, "[%s] esp_eth_new_netif_glue falhou", config->if_desc);
        return NULL;
    }
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_str_to_ip4(config->ip_addr, &ip_info.ip);
    esp_netif_str_to_ip4(config->netmask_addr, &ip_info.netmask);
    esp_netif_str_to_ip4(config->gw_addr, &ip_info.gw);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ip_info));

    // Handlers específicos desta instância (arg = if_desc, usado só para log)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, (void *)config->if_desc, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, (void *)config->if_desc, NULL));

    ESP_LOGI(TAG, "[%s] Starting ETH driver...", config->if_desc);
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "[%s] MAC = %02X:%02X:%02X:%02X:%02X:%02X", config->if_desc,
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

    return eth_netif;
}
