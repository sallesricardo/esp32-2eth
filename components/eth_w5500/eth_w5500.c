#include "eth_w5500.h"

#include <stdlib.h>
#include <string.h>

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

// Contexto persistente de uma instância W5500, alocado no heap (não no
// stack de quem chama eth_w5500_init) -- os event handlers ficam vivos
// pelo resto da execução do programa e são chamados de forma assíncrona
// bem depois de eth_w5500_init() retornar (inclusive depois de app_main()
// retornar, já que a config passada pra eth_w5500_init é local a ela).
// Guardar só se_desc (literal, sempre válido) seria suficiente pro log,
// mas pra REAPLICAR mac/IP no reconnect também precisamos do resto.
typedef struct {
    char if_desc[16];
    uint8_t mac_addr[6];
    esp_netif_t *netif;
    esp_netif_ip_info_t ip_info;
} eth_w5500_ctx_t;

// --- Event handlers (por instância, via arg = eth_w5500_ctx_t*) ---

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    eth_w5500_ctx_t *ctx = (eth_w5500_ctx_t *)arg;
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        // Reaplica MAC e IP estático desta instância a cada link up
        // (inclui religar o cabo depois de um link down) -- ver doc de
        // eth_w5500_init() no header sobre por que isso é necessário
        // (o W5500 perde o MAC configurado se o chip resetar).
        esp_err_t mac_err = esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, ctx->mac_addr);
        esp_err_t ip_err = esp_netif_set_ip_info(ctx->netif, &ctx->ip_info);
        if (mac_err != ESP_OK || ip_err != ESP_OK) {
            ESP_LOGW(TAG, "[%s] Falha ao reaplicar MAC/IP no link up (mac=%s, ip=%s)",
                     ctx->if_desc, esp_err_to_name(mac_err), esp_err_to_name(ip_err));
        }

        uint8_t mac_addr[6] = {0};
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "[%s] Link Up, MAC %02x:%02x:%02x:%02x:%02x:%02x, IP " IPSTR,
                 ctx->if_desc, mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5], IP2STR(&ctx->ip_info.ip));
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[%s] Link Down", ctx->if_desc);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "[%s] Started", ctx->if_desc);
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "[%s] Stopped", ctx->if_desc);
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    (void)event_id;
    eth_w5500_ctx_t *ctx = (eth_w5500_ctx_t *)arg;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "[%s] Got IP: " IPSTR "  MASK: " IPSTR "  GW: " IPSTR,
             ctx->if_desc, IP2STR(&ip_info->ip), IP2STR(&ip_info->netmask), IP2STR(&ip_info->gw));
}

esp_netif_t *eth_w5500_init(const eth_w5500_config_param_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "config NULL");
        return NULL;
    }

    eth_w5500_ctx_t *ctx = calloc(1, sizeof(eth_w5500_ctx_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "[%s] Falha de memoria ao alocar contexto", config->if_desc);
        return NULL;
    }
    strlcpy(ctx->if_desc, config->if_desc, sizeof(ctx->if_desc));

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
        free(ctx);
        return NULL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    // --- MAC address: o W5500 não tem MAC de fábrica, então definimos
    // explicitamente a partir do MAC base do chip, com offset para
    // diferenciar as duas instâncias W5500. Sem isso as duas placas sobem
    // com o mesmo MAC "placeholder" do driver. Guardamos em ctx->mac_addr
    // pra poder reaplicar em todo ETHERNET_EVENT_CONNECTED (ver eth_event_handler). ---
    ESP_ERROR_CHECK(esp_read_mac(ctx->mac_addr, ESP_MAC_ETH));
    ctx->mac_addr[5] = (uint8_t)(ctx->mac_addr[5] + config->mac_addr_offset);
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, ctx->mac_addr));

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
        free(ctx);
        return NULL;
    }
    ctx->netif = eth_netif;

    esp_netif_dns_info_t dns_info = {0};
    esp_netif_str_to_ip4(config->dns_addr, &dns_info.ip.u_addr.ip4);
    esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info);

    void *glue = esp_eth_new_netif_glue(eth_handle);
    if (glue == NULL) {
        ESP_LOGE(TAG, "[%s] esp_eth_new_netif_glue falhou", config->if_desc);
        free(ctx);
        return NULL;
    }
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));

    // Guardamos o ip_info em ctx (não só aplicamos uma vez) pra poder
    // reaplicar em todo ETHERNET_EVENT_CONNECTED -- ver eth_event_handler.
    esp_netif_str_to_ip4(config->ip_addr, &ctx->ip_info.ip);
    esp_netif_str_to_ip4(config->netmask_addr, &ctx->ip_info.netmask);
    esp_netif_str_to_ip4(config->gw_addr, &ctx->ip_info.gw);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ctx->ip_info));

    // Handlers específicos desta instância -- arg = ctx (alocado no heap,
    // sobrevive à função que chamou eth_w5500_init, ao contrário de um
    // ponteiro pra config/if_desc que morasse no stack de quem chamou).
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, (void *)ctx, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, (void *)ctx, NULL));

    ESP_LOGI(TAG, "[%s] Starting ETH driver...", config->if_desc);
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "[%s] MAC = %02X:%02X:%02X:%02X:%02X:%02X", config->if_desc,
             ctx->mac_addr[0], ctx->mac_addr[1], ctx->mac_addr[2],
             ctx->mac_addr[3], ctx->mac_addr[4], ctx->mac_addr[5]);

    return eth_netif;
}
