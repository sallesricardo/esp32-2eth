#include "sdkconfig.h"
#include <stdbool.h>
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
#include "doppler_events.h"
#include "doppler_processing.h"
#include "device_config.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "nvs_flash.h"

#if CONFIG_NETIF_BACKEND_WIFI
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#endif

static const char *TAG = "app_main";

// Símbolos gerados pelo EMBED_TXTFILES do componente mqtt_app (ver
// components/mqtt_app/CMakeLists.txt). O linker cria esses nomes a partir
// do caminho do arquivo, com _binary_, o nome do arquivo com "." -> "_", e
// _start/_end.
extern const uint8_t mqtt_ca_cert_pem_start[]     asm("_binary_ca_crt_start");
extern const uint8_t mqtt_client_cert_pem_start[] asm("_binary_client_crt_start");
extern const uint8_t mqtt_client_key_pem_start[]  asm("_binary_client_key_start");

#define TCP_WELCOME_MSG      "Conectado ao servidor TCP\n"
#define MQTT_TCP_NOTIFY_TOPIC      "device/tcp_server/client_connected"
#define MQTT_TCP_CLIENT_DATA_TOPIC "device/tcp_client/data_hex"
#define MQTT_CONFIG_TOPIC          "device/config/set"

#define REMOTE_HOST_IP   CONFIG_REMOTE_HOST_IP
#define REMOTE_HOST_PORT CONFIG_REMOTE_HOST_PORT

// Chamado pelo tcp_server_app assim que aceita uma nova conexão.
// Não conhece nada de MQTT diretamente sobre TCP -> quem faz a ponte é o main.
static void on_tcp_client_connected(const char *client_ip)
{
    ESP_LOGI(TAG, "Cliente TCP conectado: %s -> publicando no MQTT", client_ip);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "cJSON_CreateObject falhou (sem memoria?), notificacao MQTT descartada");
        return;
    }
    cJSON_AddStringToObject(root, "event", "tcp_client_connected");
    cJSON_AddStringToObject(root, "ip", client_ip); // escapado automaticamente pelo cJSON

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted falhou, notificacao MQTT descartada");
        return;
    }

    mqtt_app_publish(MQTT_TCP_NOTIFY_TOPIC, payload, /*qos=*/1, /*retain=*/0);
    cJSON_free(payload); // cJSON_PrintUnformatted aloca via cJSON_malloc; libera com cJSON_free
}

// Chamado pelo mqtt_app (via mqtt_app_data_cb_t) toda vez que chega uma
// mensagem em MQTT_CONFIG_TOPIC -- inclusive mensagens retidas entregues
// logo após a subscription, e reenvios do publicador. device_config só
// grava no NVS se o conteúdo mudou (hash), então repetição não desgasta
// flash à toa.
//
// Roda na task interna do esp-mqtt (ver doc de mqtt_app_data_cb_t) -- por
// enquanto só grava bruto no NVS, então é rápido o bastante pra rodar
// direto aqui. Se o parse/aplicação da config (schema ainda não definido)
// virar algo mais pesado, considere copiar `data` e delegar pro event loop
// do proxy_events (mesmo padrão usado pelos dados do proxy TCP) em vez de
// processar direto aqui dentro.
static void on_mqtt_config_data(const char *topic, size_t topic_len, const char *data, size_t data_len)
{
    (void)topic;
    (void)topic_len;

    bool changed = false;
    esp_err_t err = device_config_apply_if_changed(data, data_len, &changed);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_config_apply_if_changed falhou: %s", esp_err_to_name(err));
        return;
    }

    if (!changed) {
        ESP_LOGI(TAG, "Config MQTT recebida (%d bytes) e igual a atual, nada aplicado", (int)data_len);
        return;
    }

    ESP_LOGI(TAG, "Config MQTT nova salva no NVS (%d bytes)", (int)data_len);
    // TODO: assim que o schema das configs estiver definido, faça o parse
    // (ex: cJSON_ParseWithLength(data, data_len)) e aplique os valores nos
    // componentes relevantes aqui. Hoje device_config só guarda o bruto.
}

// --- Funções de processamento dos dados do proxy (só logging por enquanto) ---
//
// Rodam na task do event loop dedicado (proxy_events), NÃO na task de I/O
// de rede que recebeu o dado — então podem ficar mais pesadas no futuro
// sem travar a recepção do socket.

// Converte `len` bytes de `data` numa string hexadecimal minúscula (2
// chars por byte + '\0'). Retorna buffer alocado no heap — quem chama é
// dono e deve dar free(). Retorna NULL em falha de alocação ou len == 0.
static char *bytes_to_hex(const uint8_t *data, size_t len)
{
    static const char hex_digits[] = "0123456789abcdef";
    if (len == 0) {
        return NULL;
    }
    char *out = malloc(len * 2 + 1);
    if (out == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex_digits[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex_digits[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
    return out;
}

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
// TCP local. Publica o conteúdo em hex num tópico MQTT, além de encaminhar
// pro cliente TCP local (feito pelo handler do evento, logo abaixo).
static void process_data_from_remote_host(const uint8_t *data, size_t len, int64_t latency_us)
{
    ESP_LOGI(TAG, "process_data_from_remote_host: %d bytes, latencia recv->processamento: %lld us",
             (int)len, (long long)latency_us);

    char *hex = bytes_to_hex(data, len);
    if (hex == NULL) {
        ESP_LOGE(TAG, "bytes_to_hex falhou (sem memoria ou len=0), publicacao MQTT descartada");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "cJSON_CreateObject falhou (sem memoria?), publicacao MQTT descartada");
        free(hex);
        return;
    }
    cJSON_AddStringToObject(root, "event", "tcp_client_data");
    cJSON_AddStringToObject(root, "hex", hex); // cJSON copia a string internamente
    cJSON_AddNumberToObject(root, "len", (double)len);
    free(hex);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted falhou, publicacao MQTT descartada");
        return;
    }

    mqtt_app_publish(MQTT_TCP_CLIENT_DATA_TOPIC, payload, /*qos=*/1, /*retain=*/0);
    cJSON_free(payload); // cJSON_Print* aloca via cJSON_malloc -> libera com cJSON_free
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

// Callback chamado pelo tcp_client_app na própria task de I/O de rede
// (dentro do parser, assim que um pacote do radar é decodificado e o
// checksum confere) -- fica leve de propósito, só copia + enfileira nos
// event loops dedicados, sem processar aqui.
//
// `timestamp_us` vem do preâmbulo do pacote (byte de header, capturado no
// parser do tcp_client_app) -- repassamos pros dois event loops sem
// recalcular, pra medir a latência de ponta a ponta em ambos.
static void on_data_from_remote_host(uint8_t command, uint8_t frame_number,
                                      const uint8_t *payload, size_t payload_len,
                                      int64_t timestamp_us)
{
    (void)frame_number;

    doppler_events_post_data(command, payload, payload_len, timestamp_us);

    // O proxy (bytes crus pro cliente TCP local) mantém seu próprio
    // timestamp, capturado no instante da cópia (ver proxy_events.c) --
    // não é a mesma medição que a do doppler (que usa o preâmbulo).
    proxy_events_post_data(
        PROXY_EVENT_DATA_FROM_REMOTE_HOST,
        payload,
        payload_len
    );
}

#if CONFIG_NETIF_BACKEND_WIFI
// --- Modo de teste de bancada (sem W5500) ---
//
// Sobe UMA interface Wi-Fi STA e a usa tanto pro papel de ETH1 (MQTT) quanto
// ETH2 (proxy TCP) — a ESP32 clássica só tem um rádio, então não dá pra
// simular duas interfaces físicas de verdade aqui. Serve só pra exercitar a
// lógica de app (mqtt_app, tcp_server_app, tcp_client_app, proxy_events)
// sem depender do hardware W5500. Trocar de volta pra produção é só voltar
// NETIF_BACKEND pra W5500 no menuconfig — nenhum código de app foi alterado.
#define WIFI_TEST_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_test_event_group;

static void wifi_test_event_handler(void *arg, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "[teste] Wi-Fi desconectado, tentando reconectar...");
        xEventGroupClearBits(s_wifi_test_event_group, WIFI_TEST_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "[teste] Wi-Fi conectado, IP obtido.");
        xEventGroupSetBits(s_wifi_test_event_group, WIFI_TEST_CONNECTED_BIT);
    }
}

static esp_netif_t *wifi_test_init(void)
{
    s_wifi_test_event_group = xEventGroupCreate();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_test_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_test_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_TEST_WIFI_SSID,
            .password = CONFIG_TEST_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGW(TAG, "*** MODO DE TESTE: 1 interface Wi-Fi simulando ETH1+ETH2 (sem W5500) ***");
    ESP_LOGI(TAG, "[teste] Conectando ao Wi-Fi '%s'...", CONFIG_TEST_WIFI_SSID);
    xEventGroupWaitBits(s_wifi_test_event_group, WIFI_TEST_CONNECTED_BIT,
                         pdFALSE, pdTRUE, portMAX_DELAY);

    return sta_netif;
}
#endif // CONFIG_NETIF_BACKEND_WIFI

void app_main(void)
{
    // Padrão usual do IDF: se a partição NVS estiver nova/com layout
    // antigo, apaga e reinicializa. Precisa rodar ANTES de
    // device_config_init() (que abre um namespace NVS) e, no backend de
    // teste, antes de esp_wifi_init() (que também usa NVS pra guardar
    // calibração de rádio).
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(device_config_init());

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *eth_netif_1;
    esp_netif_t *eth_netif_2;

#if CONFIG_NETIF_BACKEND_WIFI
    esp_netif_t *test_netif = wifi_test_init();
    eth_netif_1 = test_netif;
    eth_netif_2 = test_netif;
#else
    // --- W5500 #1: usado para MQTT ---
    // spi_host vem do Kconfig (CONFIG_ETH1_W5500_SPI_HOST: 1=SPI2_HOST/HSPI,
    // 2=SPI3_HOST/VSPI) para permitir inverter as portas sem recompilar o
    // mapeamento fixo no código.
    eth_w5500_config_param_t eth1_cfg = {
        .spi_host        = (spi_host_device_t)CONFIG_ETH1_W5500_SPI_HOST,
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
        .spi_host        = (spi_host_device_t)CONFIG_ETH2_W5500_SPI_HOST,
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

    // Cada instância chama spi_bus_initialize() para o seu spi_host; se
    // ETH1 e ETH2 apontarem pro mesmo host (erro de menuconfig), a segunda
    // chamada falha. Checar aqui dá um erro claro em vez de um
    // ESP_ERROR_CHECK genérico lá dentro do driver.
    if (eth1_cfg.spi_host == eth2_cfg.spi_host) {
        ESP_LOGE(TAG, "ETH1 e ETH2 configurados no mesmo SPI host (%d); "
                       "ajuste CONFIG_ETH1_W5500_SPI_HOST / CONFIG_ETH2_W5500_SPI_HOST "
                       "no menuconfig para hosts diferentes.", (int)eth1_cfg.spi_host);
        return;
    }

    ESP_LOGI(TAG, "Initializing W5500 #1 (MQTT)...");
    eth_netif_1 = eth_w5500_init(&eth1_cfg);

    ESP_LOGI(TAG, "Initializing W5500 #2 (TCP Server)...");
    eth_netif_2 = eth_w5500_init(&eth2_cfg);
#endif // CONFIG_NETIF_BACKEND_WIFI

    if (eth_netif_1 == NULL || eth_netif_2 == NULL) {
        ESP_LOGE(TAG, "Falha ao inicializar uma das interfaces Ethernet, abortando.");
        return;
    }

    // Aplica (por enquanto, só loga) o último config recebido antes do
    // reboot -- assim o device não fica "sem config" enquanto espera o
    // MQTT reconectar e o broker reenviar (se for retained) ou você
    // republicar manualmente.
    {
        static char boot_cfg_buf[DEVICE_CONFIG_MAX_LEN];
        size_t boot_cfg_len = 0;
        esp_err_t err = device_config_load_raw(boot_cfg_buf, sizeof(boot_cfg_buf), &boot_cfg_len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Config salva encontrada no NVS (%d bytes) -- TODO: aplicar quando o schema existir",
                     (int)boot_cfg_len);
        }
        // ESP_ERR_NVS_NOT_FOUND (primeiro boot) é esperado e já foi logado
        // dentro de device_config_load_raw; outros erros também.
    }

    static const mqtt_app_subscription_t mqtt_subscriptions[] = {
        { .topic = MQTT_CONFIG_TOPIC, .qos = 1 },
    };
    mqtt_app_config_t mqtt_cfg = {
        .eth_netif = eth_netif_1,
        .ca_cert_pem = (const char *)mqtt_ca_cert_pem_start,
        .client_cert_pem = (const char *)mqtt_client_cert_pem_start,
        .client_key_pem = (const char *)mqtt_client_key_pem_start,
        .subscriptions = mqtt_subscriptions,
        .subscriptions_count = sizeof(mqtt_subscriptions) / sizeof(mqtt_subscriptions[0]),
        .on_data = on_mqtt_config_data,
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
    // Doppler: event loop dedicado, separado do loop default (ETH_EVENT/IP_EVENT).
    // O roteamento por comando (event_id) e o processamento de cada um
    // vivem em components/doppler_processing -- adicionar um comando novo
    // não mexe aqui, só na tabela s_handlers de lá.
    ESP_ERROR_CHECK(doppler_events_init());
    ESP_ERROR_CHECK(doppler_processing_init());


    tcp_server_app_start(CONFIG_TCP_SERVER_PORT, proxy_bind_netif, TCP_WELCOME_MSG,
                          on_tcp_client_connected, on_data_from_tcp_client);
    tcp_client_app_start(REMOTE_HOST_IP, REMOTE_HOST_PORT, proxy_bind_netif, on_data_from_remote_host);
}
