#pragma once

#include "esp_netif.h"
#include "driver/spi_master.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuração completa de uma instância W5500.
 *
 * Uma struct por porta física, para não repetir 13 parâmetros posicionais
 * em cada chamada (como no app_main.c original).
 */
typedef struct {
    // --- SPI / GPIO ---
    spi_host_device_t spi_host;
    int miso_gpio;
    int mosi_gpio;
    int sclk_gpio;
    int cs_gpio;
    int int_gpio;
    int rst_gpio;
    int clock_speed_hz;      // ex: 20 * 1000 * 1000 (ajuste conforme layout)

    // --- Rede (IP estático) ---
    const char *ip_addr;
    const char *gw_addr;
    const char *netmask_addr;
    const char *dns_addr;

    // --- esp_netif ---
    const char *if_key;      // ex: "ETH1"
    const char *if_desc;     // ex: "eth1"
    int route_prio;          // maior = preferido como netif padrão

    /**
     * Offset aplicado ao último byte do MAC base do chip (ESP_MAC_ETH).
     * OBRIGATÓRIO ser diferente entre as duas instâncias W5500, já que o
     * chip não tem MAC de fábrica e o driver usa um MAC placeholder fixo
     * caso você não defina um explicitamente. Ex: instância 1 -> 0, instância 2 -> 1.
     */
    uint8_t mac_addr_offset;
} eth_w5500_config_param_t;

/**
 * @brief Inicializa uma instância W5500 (barramento SPI, MAC/PHY, netif com IP estático).
 *
 * Registra internamente os event handlers ETH_EVENT e IP_EVENT_ETH_GOT_IP
 * para esta instância específica, já logando com o if_desc correto.
 *
 * A cada ETHERNET_EVENT_CONNECTED (link up -- inclusive religar o cabo depois
 * de desconectado), reaplica o MAC e o IP estático corretos DESTA instância
 * no handle que gerou o evento. Isso existe porque o W5500 guarda o MAC num
 * registrador volátil (SHAR) que não sobrevive a um reset de hardware do
 * chip -- então se o link cair de um jeito que reseta o chip (glitch de
 * energia, RST), ele volta com MAC/IP não configurados até alguém
 * reaplicar. Sem isso, a instância pode voltar respondendo com uma
 * config errada (inclusive a de outra porta, se sobrar lixo de memória)
 * até o próximo reboot do ESP32.
 *
 * @param config Configuração da instância (ver eth_w5500_config_param_t)
 * @return Handle do esp_netif criado, ou NULL em caso de falha (com log de erro).
 */
esp_netif_t *eth_w5500_init(const eth_w5500_config_param_t *config);

#ifdef __cplusplus
}
#endif
