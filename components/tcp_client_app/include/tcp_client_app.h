#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback chamado sempre que um pacote do radar é decodificado com
 *        sucesso (checksum ok) a partir dos dados recebidos do host remoto.
 *
 * Roda na task de I/O do tcp_client_app (a mesma que faz o recv()) — fique
 * leve aqui: copie os bytes e delegue o processamento pesado pra um event
 * loop dedicado (ex: doppler_events_post_data), não processe direto neste
 * callback. O buffer `payload` é binário (não é uma string terminada em
 * \0) e só é válido durante a chamada — copie o que precisar se for usar
 * depois de retornar.
 *
 * @param command Código de comando do pacote recebido.
 * @param frame_number Número do pacote recebido.
 * @param payload Ponteiro para os bytes do payload recebido (só o payload,
 *                sem header/tamanho/comando/frame/checksum/footer).
 * @param payload_len Quantidade de bytes em `payload`.
 * @param timestamp_us Instante (esp_timer_get_time(), microssegundos desde
 *                o boot) em que o PREÂMBULO do pacote (o byte de header,
 *                0xDB) chegou pela rede — capturado o mais cedo possível
 *                dentro da máquina de estados do parser, não no fim do
 *                pacote. Repasse esse valor adiante (ex: pra
 *                doppler_events_post_data) em vez de tirar um novo
 *                timestamp aqui, pra medir a latência de ponta a ponta:
 *                do primeiro byte do pacote até o fim do processamento.
 */
typedef void (*tcp_client_data_cb_t)(uint8_t command, uint8_t frame_number,
                                      const uint8_t *payload, size_t payload_len,
                                      int64_t timestamp_us);

/**
 * @brief Cria a task que conecta (como cliente TCP) a um host remoto e
 *        mantém a conexão, reconectando automaticamente se cair.
 *
 * A cada dado recebido do host remoto, chama on_data_received (se não for
 * NULL) — útil para encaminhar os dados a outro lugar (ex: o cliente TCP
 * conectado no tcp_server_app, fechando o proxy) sem esse componente
 * precisar conhecer o destino.
 *
 * @param host Hostname ou IP do host remoto (ex: "192.168.1.100").
 * @param port Porta TCP do host remoto (ex: 50000).
 * @param bind_netif Se não for NULL, o socket faz bind() no IP dessa
 *                    interface antes do connect() — força a saída por ela
 *                    independente do que a tabela de rotas escolheria.
 *                    Se for NULL, deixa o roteamento decidir (comportamento
 *                    antigo).
 * @param on_data_received Callback opcional (pode ser NULL) chamado a cada
 *                    dado recebido do host remoto.
 */
void tcp_client_app_start(const char *host,
                           uint16_t port,
                           esp_netif_t *bind_netif,
                           tcp_client_data_cb_t on_data_received);

/**
 * @brief Envia dados para o host remoto, se a conexão estiver ativa.
 *
 * Thread-safe: pode ser chamada de outra task (ex: da task do
 * tcp_server_app, para fazer o proxy no sentido cliente local -> remoto).
 *
 * @param data Bytes a enviar.
 * @param len Quantidade de bytes.
 * @return true se enviado com sucesso, false se não há conexão ativa
 *         ou se ocorreu erro no envio.
 */
bool tcp_client_app_send(const uint8_t *data, size_t len);

/**
 * @brief Monta um pacote no formato do protocolo do radar (header, tamanho,
 *        comando, número de frame, payload, checksum, footer — ver parser
 *        em tcp_client_app.c) e envia via tcp_client_app_send().
 *
 * Numera o frame automaticamente (contador interno, incrementado a cada
 * envio). Use pra mandar comandos próprios pro radar; para simplesmente
 * repassar bytes já recebidos (proxy), use tcp_client_app_send() direto.
 *
 * @param command Código de comando a enviar.
 * @param payload Payload a enviar (pode ser NULL se payload_len for 0).
 * @param payload_len Tamanho do payload. Deve caber no limite interno do
 *                parser (ver MAX_PAYLOAD_SIZE em tcp_client_app.c).
 * @return true se enviado com sucesso, false em erro de alocação, payload
 *         maior que o limite, ou falha no envio (sem conexão ativa etc.).
 */
bool tcp_client_app_send_command(uint8_t command, const uint8_t *payload, uint16_t payload_len);

#ifdef __cplusplus
}
#endif
