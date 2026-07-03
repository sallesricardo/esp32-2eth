#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "driver/spi_master.h"

#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"

static const char *TAG = "W5500";

/* Ajuste para sua placa */
#define ETH1_SPI_HOST       SPI2_HOST
#define ETH1_MISO_GPIO      CONFIG_ETH1_W5500_SPI_MISO
#define ETH1_MOSI_GPIO      CONFIG_ETH1_W5500_SPI_MOSI
#define ETH1_SCLK_GPIO      CONFIG_ETH1_W5500_SPI_SCLK
#define ETH1_CS_GPIO        CONFIG_ETH1_W5500_SPI_CS
#define ETH1_INT_GPIO       CONFIG_ETH1_W5500_INT
#define ETH1_RST_GPIO       CONFIG_ETH1_W5500_RST

#define PIN_MOSI   ETH1_MOSI_GPIO
#define PIN_MISO   ETH1_MISO_GPIO
#define PIN_SCLK   ETH1_SCLK_GPIO
#define PIN_CS     ETH1_CS_GPIO
#define PIN_INT    ETH1_INT_GPIO
#define PIN_RST    ETH1_RST_GPIO

static esp_eth_handle_t eth_handle;

/********************************************************************/

static void eth_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    uint8_t mac[6];

    switch (event_id)
    {
        case ETHERNET_EVENT_START:

            ESP_LOGI(TAG, "Ethernet Started");

            esp_eth_ioctl(
                eth_handle,
                ETH_CMD_G_MAC_ADDR,
                mac);

            ESP_LOGI(TAG,
                     "MAC = %02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2],
                     mac[3], mac[4], mac[5]);

            break;

        case ETHERNET_EVENT_CONNECTED:

            ESP_LOGI(TAG, "LINK UP");

            esp_eth_ioctl(
                eth_handle,
                ETH_CMD_G_MAC_ADDR,
                mac);

            ESP_LOGI(TAG,
                     "MAC = %02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2],
                     mac[3], mac[4], mac[5]);

            break;

        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "LINK DOWN");
            break;

        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;
    }
}

/********************************************************************/

static void got_ip_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    ip_event_got_ip_t *event =
        (ip_event_got_ip_t *)event_data;

    ESP_LOGI(TAG, "GOT IP");

    ESP_LOGI(TAG,
             "IP      : " IPSTR,
             IP2STR(&event->ip_info.ip));

    ESP_LOGI(TAG,
             "MASK    : " IPSTR,
             IP2STR(&event->ip_info.netmask));

    ESP_LOGI(TAG,
             "GATEWAY : " IPSTR,
             IP2STR(&event->ip_info.gw));
}

/********************************************************************/

void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(
        esp_event_loop_create_default());

    /**************** SPI ****************/

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    ESP_ERROR_CHECK(
        spi_bus_initialize(
            SPI_HOST,
            &buscfg,
            SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 10 * 1000 * 1000,
        .spics_io_num = PIN_CS,
        .queue_size = 20,
    };

    /**************** W5500 ****************/

    eth_w5500_config_t w5500_config =
        ETH_W5500_DEFAULT_CONFIG(
            SPI_HOST,
            &devcfg);

    w5500_config.int_gpio_num = PIN_INT;

    eth_mac_config_t mac_config =
        ETH_MAC_DEFAULT_CONFIG();

    eth_phy_config_t phy_config =
        ETH_PHY_DEFAULT_CONFIG();

    phy_config.reset_gpio_num = PIN_RST;

    esp_eth_mac_t *mac =
        esp_eth_mac_new_w5500(
            &w5500_config,
            &mac_config);

    esp_eth_phy_t *phy =
        esp_eth_phy_new_w5500(
            &phy_config);

    esp_eth_config_t config =
        ETH_DEFAULT_CONFIG(mac, phy);

    ESP_ERROR_CHECK(
        esp_eth_driver_install(
            &config,
            &eth_handle));

    /**************** NETIF ****************/

    esp_netif_config_t netif_cfg =
        ESP_NETIF_DEFAULT_ETH();

    esp_netif_t *netif =
        esp_netif_new(&netif_cfg);

    assert(netif);

    ESP_ERROR_CHECK(
        esp_netif_attach(
            netif,
            esp_eth_new_netif_glue(
                eth_handle)));

    /**************** EVENTS ****************/

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            ETH_EVENT,
            ESP_EVENT_ANY_ID,
            &eth_event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_ETH_GOT_IP,
            &got_ip_handler,
            NULL));

    /**************** START ****************/

    ESP_ERROR_CHECK(
        esp_eth_start(
            eth_handle));

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}