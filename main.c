#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include <inttypes.h>

/* ========== CONFIGURAÇÕES ========== */
#define QUEUE_LENGTH            10
#define QUEUE_ITEM_SIZE         sizeof(int)
#define TWDT_TIMEOUT_S          5

#define GENERATOR_TASK_PRIO     5
#define RECEIVER_TASK_PRIO      4
#define SUPERVISOR_TASK_PRIO    6

#define GENERATOR_STACK_SIZE    3072
#define RECEIVER_STACK_SIZE     4096
#define SUPERVISOR_STACK_SIZE   3072

/* Timeouts e limites */
#define QUEUE_SEND_TIMEOUT_MS   0      // Não bloqueia se fila cheia
#define QUEUE_RECV_TIMEOUT_MS   2000   // Timeout para recepção
#define SUPERVISOR_PERIOD_MS    3000
#define MAX_WARNINGS            3
#define MAX_RECOVERIES          5
#define MAX_SHUTDOWNS           10

/* Event Group Flags */
#define FLAG_GENERATOR_OK       BIT0
#define FLAG_RECEIVER_OK        BIT1
#define FLAG_RECEIVER_WARNING   BIT2
#define FLAG_RECEIVER_RECOVERY  BIT3
#define FLAG_RECEIVER_SHUTDOWN  BIT4

/* Identificador personalizado */
#define USER_ID "{Lucas-RM86920}"

/* Tags personalizadas */
#define TAG_GEN USER_ID " [GERADOR]"
#define TAG_RCV USER_ID " [RECEPTOR]"
#define TAG_SUP USER_ID " [SUPERVISOR]"
#define TAG_QUEUE USER_ID " [FILA]"
#define TAG_WDT USER_ID " [WATCHDOG]"
#define TAG_MEM USER_ID " [MEMORIA]"
#define TAG_MAIN USER_ID " [SISTEMA]"

/* ========== VARIÁVEIS GLOBAIS ========== */
static QueueHandle_t data_queue = NULL;
static EventGroupHandle_t status_flags = NULL;
static TaskHandle_t generator_task_handle = NULL;
static TaskHandle_t receiver_task_handle = NULL;

/* Heartbeats para monitoramento */
static volatile uint32_t generator_heartbeat = 0;
static volatile uint32_t receiver_heartbeat = 0;

/* ========== MÓDULO 1: GERAÇÃO DE DADOS ========== */
void task_data_generator(void *pvParameters) {
    // Inscreve a tarefa no Watchdog
    esp_task_wdt_add(NULL);
    
    int sequential_value = 0;
    
    printf("%s Módulo de Geração iniciado\n", TAG_GEN);
    
    for (;;) {
        sequential_value++;
        
        // Tenta enviar para a fila sem bloquear
        if (xQueueSend(data_queue, &sequential_value, pdMS_TO_TICKS(QUEUE_SEND_TIMEOUT_MS)) == pdTRUE) {
            printf("%s Dado enviado com sucesso!\n", TAG_QUEUE);
            printf("%s Valor %d gerado e adicionado à fila\n", TAG_GEN, sequential_value);
            
            // Atualiza flag de status
            xEventGroupSetBits(status_flags, FLAG_GENERATOR_OK);
            generator_heartbeat = xTaskGetTickCount();
        } else {
            // Fila cheia - descarta valor mas continua funcionando
            printf("%s Fila cheia! Dado descartado\n", TAG_QUEUE);
            printf("%s AVISO: Valor %d descartado (fila lotada)\n", TAG_GEN, sequential_value);
        }
        
        // Reseta o watchdog
        esp_task_wdt_reset();
        
        // Delay entre gerações (200ms)
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ========== MÓDULO 2: RECEPÇÃO DE DADOS ========== */
void task_data_receiver(void *pvParameters) {
    // Inscreve a tarefa no Watchdog
    esp_task_wdt_add(NULL);
    
    int timeout_count = 0;
    int warning_count = 0;
    int recovery_count = 0;
    int shutdown_count = 0;
    
    printf("%s Módulo de Recepção iniciado\n", TAG_RCV);
    
    for (;;) {
        // Aloca memória dinamicamente para armazenar o valor
        int *received_value = (int *)malloc(sizeof(int));
        
        if (received_value == NULL) {
            printf("%s ERRO CRÍTICO: Falha na alocação de memória!\n", TAG_MEM);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // Tenta receber dados da fila com timeout
        if (xQueueReceive(data_queue, received_value, pdMS_TO_TICKS(QUEUE_RECV_TIMEOUT_MS)) == pdTRUE) {
            // Sucesso na recepção
            printf("%s Dado recebido da fila\n", TAG_QUEUE);
            printf("%s >>> TRANSMITINDO: %d <<<\n", TAG_RCV, *received_value);
            
            // Reset dos contadores
            timeout_count = 0;
            warning_count = 0;
            recovery_count = 0;
            
            // Atualiza flags
            xEventGroupSetBits(status_flags, FLAG_RECEIVER_OK);
            xEventGroupClearBits(status_flags, FLAG_RECEIVER_WARNING | FLAG_RECEIVER_RECOVERY | FLAG_RECEIVER_SHUTDOWN);
            
            receiver_heartbeat = xTaskGetTickCount();
            
        } else {
            // Timeout - não recebeu dados
            timeout_count++;
            printf("%s TIMEOUT: Nenhum dado recebido na fila (tentativa %d)\n", TAG_RCV, timeout_count);
            
            // REAÇÃO ESCALONADA
            if (timeout_count >= 1 && timeout_count < MAX_WARNINGS) {
                // Nível 1: Avisos
                warning_count++;
                printf("%s [NIVEL 1 - AVISO %d/%d] Fila sem dados\n", TAG_RCV, warning_count, MAX_WARNINGS);
                xEventGroupSetBits(status_flags, FLAG_RECEIVER_WARNING);
                
            } else if (timeout_count >= MAX_WARNINGS && timeout_count < MAX_RECOVERIES) {
                // Nível 2: Tentativa de recuperação
                recovery_count++;
                printf("%s [NIVEL 2 - RECUPERAÇÃO %d/%d] Resetando fila e limpando buffers\n", 
                       TAG_RCV, recovery_count, MAX_RECOVERIES);
                xQueueReset(data_queue);
                xEventGroupSetBits(status_flags, FLAG_RECEIVER_RECOVERY);
                xEventGroupClearBits(status_flags, FLAG_RECEIVER_WARNING);
                
            } else if (timeout_count >= MAX_RECOVERIES && timeout_count < MAX_SHUTDOWNS) {
                // Nível 3: Preparação para encerramento
                shutdown_count++;
                printf("%s [NIVEL 3 - CRÍTICO %d/%d] Preparando para encerramento\n", 
                       TAG_RCV, shutdown_count, MAX_SHUTDOWNS);
                xEventGroupSetBits(status_flags, FLAG_RECEIVER_SHUTDOWN);
                xEventGroupClearBits(status_flags, FLAG_RECEIVER_WARNING | FLAG_RECEIVER_RECOVERY);
                
            } else {
                // Nível 4: Encerramento da tarefa
                printf("%s [NIVEL 4 - ENCERRAMENTO] Falha persistente detectada\n", TAG_RCV);
                printf("%s Finalizando módulo de recepção\n", TAG_RCV);
                free(received_value);
                xEventGroupSetBits(status_flags, FLAG_RECEIVER_SHUTDOWN);
                vTaskDelete(NULL);
                return;
            }
        }
        
        // Libera memória alocada
        free(received_value);
        
        // Reseta o watchdog
        esp_task_wdt_reset();
        
        // Pequeno delay
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ========== MÓDULO 3: SUPERVISÃO ========== */
void task_supervisor(void *pvParameters) {
    int receiver_restart_count = 0;
    
    printf("%s Módulo de Supervisão iniciado\n", TAG_SUP);
    
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_PERIOD_MS));
        
        // Lê as flags de status
        EventBits_t flags = xEventGroupGetBits(status_flags);
        
        printf("\n%s ========== STATUS DO SISTEMA ==========\n", TAG_SUP);
        
        // Status do Gerador
        if (flags & FLAG_GENERATOR_OK) {
            printf("%s Módulo Gerador: [OK] - Funcionando normalmente\n", TAG_SUP);
        } else {
            printf("%s Módulo Gerador: [FALHA] - Sem resposta\n", TAG_SUP);
        }
        
        // Status do Receptor
        if (flags & FLAG_RECEIVER_OK) {
            printf("%s Módulo Receptor: [OK] - Recebendo dados\n", TAG_SUP);
        } else if (flags & FLAG_RECEIVER_WARNING) {
            printf("%s Módulo Receptor: [AVISO] - Timeouts detectados\n", TAG_SUP);
        } else if (flags & FLAG_RECEIVER_RECOVERY) {
            printf("%s Módulo Receptor: [RECUPERAÇÃO] - Tentando recuperar\n", TAG_SUP);
        } else if (flags & FLAG_RECEIVER_SHUTDOWN) {
            printf("%s Módulo Receptor: [CRÍTICO] - Em processo de encerramento\n", TAG_SUP);
        } else {
            printf("%s Módulo Receptor: [DESCONHECIDO] - Status indeterminado\n", TAG_SUP);
        }
        
        // Informações de memória
        size_t free_heap = xPortGetFreeHeapSize();
        size_t min_heap = xPortGetMinimumEverFreeHeapSize();
        printf("%s Memória livre: %u bytes (mínimo histórico: %u bytes)\n", 
               TAG_MEM, (unsigned int)free_heap, (unsigned int)min_heap);
        
        printf("%s ========================================\n\n", TAG_SUP);
        
        // Verifica se precisa recriar tarefa do receptor
        TickType_t now = xTaskGetTickCount();
        if (receiver_task_handle == NULL || 
            (now - receiver_heartbeat > pdMS_TO_TICKS(2 * SUPERVISOR_PERIOD_MS))) {
            
            printf("%s AÇÃO: Recriando tarefa do Receptor (tentativa %d)\n", 
                   TAG_SUP, ++receiver_restart_count);
            
            if (receiver_task_handle != NULL) {
                vTaskDelete(receiver_task_handle);
                receiver_task_handle = NULL;
            }
            
            xTaskCreatePinnedToCore(
                task_data_receiver,
                "receiver_task",
                RECEIVER_STACK_SIZE,
                NULL,
                RECEIVER_TASK_PRIO,
                &receiver_task_handle,
                1
            );
            
            receiver_heartbeat = xTaskGetTickCount();
            xEventGroupClearBits(status_flags, FLAG_RECEIVER_WARNING | FLAG_RECEIVER_RECOVERY | FLAG_RECEIVER_SHUTDOWN);
            
            // Se falhou muitas vezes, reinicia o sistema
            if (receiver_restart_count >= 5) {
                printf("%s REINICIALIZAÇÃO CRÍTICA: Falhas excessivas detectadas\n", TAG_WDT);
                printf("%s Reiniciando ESP32 em 1 segundo...\n", TAG_MAIN);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        }
        
        // Verifica gerador
        if (now - generator_heartbeat > pdMS_TO_TICKS(2 * SUPERVISOR_PERIOD_MS)) {
            printf("%s AÇÃO: Recriando tarefa do Gerador\n", TAG_SUP);
            
            if (generator_task_handle != NULL) {
                vTaskDelete(generator_task_handle);
                generator_task_handle = NULL;
            }
            
            xTaskCreatePinnedToCore(
                task_data_generator,
                "generator_task",
                GENERATOR_STACK_SIZE,
                NULL,
                GENERATOR_TASK_PRIO,
                &generator_task_handle,
                1
            );
            
            generator_heartbeat = xTaskGetTickCount();
        }
        
        // Alerta de memória crítica
        if (min_heap < 10 * 1024) {
            printf("%s ALERTA CRÍTICO: Memória mínima muito baixa!\n", TAG_MEM);
        }
    }
}

/* ========== FUNÇÃO PRINCIPAL ========== */
void app_main(void) {
    printf("\n=================================================\n");
    printf("%s Sistema Multitarefa FreeRTOS Iniciando...\n", TAG_MAIN);
    printf("=================================================\n\n");
    
    // Cria a fila de comunicação
    data_queue = xQueueCreate(QUEUE_LENGTH, QUEUE_ITEM_SIZE);
    if (data_queue == NULL) {
        printf("%s ERRO FATAL: Falha ao criar fila\n", TAG_QUEUE);
        printf("%s Reiniciando sistema...\n", TAG_MAIN);
        esp_restart();
    }
    printf("%s Fila criada com sucesso (capacidade: %d itens)\n", TAG_QUEUE, QUEUE_LENGTH);
    
    // Cria o Event Group para flags de status
    status_flags = xEventGroupCreate();
    if (status_flags == NULL) {
        printf("%s ERRO FATAL: Falha ao criar event group\n", TAG_MAIN);
        printf("%s Reiniciando sistema...\n", TAG_MAIN);
        esp_restart();
    }
    printf("%s Event Group criado com sucesso\n", TAG_MAIN);
    
    // Configura e inicializa o Watchdog Timer
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = TWDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,  // Não monitora idle tasks
        .trigger_panic = true  // Causa panic e reinicia se timeout
    };
    
    esp_err_t wdt_result = esp_task_wdt_init(&twdt_config);
    if (wdt_result == ESP_OK) {
        printf("%s Watchdog Timer configurado: %d segundos\n", TAG_WDT, TWDT_TIMEOUT_S);
    } else {
        printf("%s AVISO: Falha ao configurar Watchdog Timer\n", TAG_WDT);
    }
    
    // Cria as tarefas
    printf("\n%s Criando tarefas do sistema...\n", TAG_MAIN);
    
    xTaskCreatePinnedToCore(
        task_data_generator,
        "generator_task",
        GENERATOR_STACK_SIZE,
        NULL,
        GENERATOR_TASK_PRIO,
        &generator_task_handle,
        1  // Core 1
    );
    printf("%s Tarefa Gerador criada (Core 1, Prioridade %d)\n", TAG_MAIN, GENERATOR_TASK_PRIO);
    
    xTaskCreatePinnedToCore(
        task_data_receiver,
        "receiver_task",
        RECEIVER_STACK_SIZE,
        NULL,
        RECEIVER_TASK_PRIO,
        &receiver_task_handle,
        1  // Core 1
    );
    printf("%s Tarefa Receptor criada (Core 1, Prioridade %d)\n", TAG_MAIN, RECEIVER_TASK_PRIO);
    
    xTaskCreatePinnedToCore(
        task_supervisor,
        "supervisor_task",
        SUPERVISOR_STACK_SIZE,
        NULL,
        SUPERVISOR_TASK_PRIO,
        NULL,
        0  // Core 0
    );
    printf("%s Tarefa Supervisor criada (Core 0, Prioridade %d)\n", TAG_MAIN, SUPERVISOR_TASK_PRIO);
    
    printf("\n%s Todas as tarefas criadas com sucesso!\n", TAG_MAIN);
    printf("%s Sistema em execução...\n\n", TAG_MAIN);
}
