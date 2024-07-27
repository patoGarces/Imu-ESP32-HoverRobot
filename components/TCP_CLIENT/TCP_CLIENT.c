#include "include/TCP_CLIENT.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define EXAMPLE_ESP_WIFI_SSID       "Speedy-Fibra-8F8D64"
#define EXAMPLE_ESP_WIFI_PASS       "39919131"
#define HOST_IP_ADDR                "192.168.1.35"
#define PORT                        8080
#define CONFIG_EXAMPLE_IPV4 1
// #define CONFIG_EXAMPLE_IPV6 1

#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK//WIFI_AUTH_WPA_WPA2_PSK//WIFI_AUTH_WPA2_PSK
#define EXAMPLE_ESP_MAXIMUM_RETRY  30


#define STREAM_BUFFER_SIZE              100
#define STREAM_BUFFER_LENGTH_TRIGGER    3
StreamBufferHandle_t xStreamBufferReceiver;
StreamBufferHandle_t xStreamBufferSender;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

static int s_retry_num = 0;

static const char *TAG = "TCP CLIENT";

uint8_t serverClientConnected = false;

static void tcpClientSocket(void *pvParameters)
{
    char rx_buffer[128];
    char host_ip[] = HOST_IP_ADDR;
    int addr_family = 0;
    int ip_protocol = 0;

    char received_data[100];

    while (1) {
#if defined(CONFIG_EXAMPLE_IPV4)
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = inet_addr(host_ip);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
#elif defined(CONFIG_EXAMPLE_IPV6)
        struct sockaddr_in6 dest_addr = { 0 };
        inet6_aton(host_ip, &dest_addr.sin6_addr);
        dest_addr.sin6_family = AF_INET6;
        dest_addr.sin6_port = htons(PORT);
        dest_addr.sin6_scope_id = esp_netif_get_netif_impl_index(EXAMPLE_INTERFACE);
        addr_family = AF_INET6;
        ip_protocol = IPPROTO_IPV6;
#elif defined(CONFIG_EXAMPLE_SOCKET_IP_INPUT_STDIN)
        struct sockaddr_storage dest_addr = { 0 };
        ESP_ERROR_CHECK(get_addr_from_stdin(PORT, SOCK_STREAM, &ip_protocol, &addr_family, &dest_addr));
#endif
        int sock =  socket(addr_family, SOCK_STREAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            // break;
        } else {
            ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, PORT);

            int errConnect = connect(sock, (struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in6));
            if (errConnect != 0) {
                ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
                // break;
            } else {
                ESP_LOGI(TAG, "Successfully connected");
                serverClientConnected = true;

                uint32_t contador = 0;
                while (1) {
                    
                    
                    // int err = lwip_send(sock, payload, strlen(payload), 0);
                    // if (err < 0) {
                    //     ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                    //     break;
                    // }

                    BaseType_t bytesStreamReceived = xStreamBufferReceive(xStreamBufferSender, received_data, sizeof(received_data), 0);

                    if (bytesStreamReceived > 1) {
                        int errSend = lwip_send(sock, received_data, bytesStreamReceived, 0);
                        if (errSend < 0) {
                            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                            break;
                        } 
                        else {
                            contador++;
                            // ESP_LOGI(TAG, "Paquete enviado, header: %x, %x, len: %d", received_data[0],received_data[1],bytesStreamReceived);
                            ESP_LOGI(TAG, "Paquetes enviados %ld", contador);
                        }
                    }

                    // int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
                    // ESP_LOGI(TAG,"post recv method");
                    // // Error occurred during receiving
                    // if (len < 0) {
                    //     ESP_LOGE(TAG, "recv failed: errno %d", errno);
                    //     break;
                    // }
                    // // Data received
                    // else {
                    //     rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
                    //     ESP_LOGI(TAG, "Received %d bytes from %s:", len, host_ip);
                    //     ESP_LOGI(TAG, "%s", rx_buffer);
                    // }



                    vTaskDelay(pdMS_TO_TICKS(25));
                }

                serverClientConnected = false;
                if (sock != -1) {
                    ESP_LOGE(TAG, "Shutting down socket and restarting...");
                    shutdown(sock, 0);
                    close(sock);
                }
            }
        }
    }
    ESP_LOGE(TAG,"TCP CLIENT SOCKET vTASKDELETE");
    vTaskDelete(NULL);
}

// static void wifi_event_handler(void* arg, esp_event_base_t event_base,
//                                     int32_t event_id, void* event_data)
// {
//     if (event_id == WIFI_EVENT_AP_STACONNECTED) {
//         wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
//         ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
//                  MAC2STR(event->mac), event->aid);
        
//         xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);
//     } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
//         wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
//         ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
//                  MAC2STR(event->mac), event->aid);
//     }
// }

// void wifi_init_softap(void)
// {
//     ESP_ERROR_CHECK(esp_netif_init());
//     ESP_ERROR_CHECK(esp_event_loop_create_default());
//     esp_netif_create_default_wifi_ap();

//     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
//     ESP_ERROR_CHECK(esp_wifi_init(&cfg));

//     ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
//                                                         ESP_EVENT_ANY_ID,
//                                                         &wifi_event_handler,
//                                                         NULL,
//                                                         NULL));

//     wifi_config_t wifi_config = {
//         .ap = {
//             .ssid = EXAMPLE_ESP_WIFI_SSID,
//             .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
//             .channel = EXAMPLE_ESP_WIFI_CHANNEL,
//             .password = EXAMPLE_ESP_WIFI_PASS,
//             .max_connection = EXAMPLE_MAX_STA_CONN,
// #ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
//             .authmode = WIFI_AUTH_WPA3_PSK,
//             .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
// #else /* CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT */
//             .authmode = WIFI_AUTH_WPA2_PSK,
// #endif
//             .pmf_cfg = {
//                     .required = true,
//             },
//         },
//     };
//     if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
//         wifi_config.ap.authmode = WIFI_AUTH_OPEN;
//     }

//     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
//     ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
//     ESP_ERROR_CHECK(esp_wifi_start());

//     ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
//              EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
// }

static void wifiAndIpEvent(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        ESP_LOGI(TAG,"iniciando tcp_client_task");
        xTaskCreatePinnedToCore(tcpClientSocket, "tcp_client", 4096, NULL,configMAX_PRIORITIES - 1, NULL,0);// COMMS_HANDLER_CORE); // TODO: agregar core configurable
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifiAndIpEvent,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifiAndIpEvent,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
	     .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}



void initTcpClient(char *serverIp) {
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ESP_LOGI(TAG, "ESP_WIFI_MODE_AP");
    // wifi_init_softap();
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    xStreamBufferSender = xStreamBufferCreate(STREAM_BUFFER_SIZE, STREAM_BUFFER_LENGTH_TRIGGER);
    xStreamBufferReceiver = xStreamBufferCreate(STREAM_BUFFER_SIZE, STREAM_BUFFER_LENGTH_TRIGGER);
}

uint8_t isTcpClientConnected(void) {
    return serverClientConnected; 
}