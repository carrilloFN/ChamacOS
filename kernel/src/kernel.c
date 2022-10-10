#include "../headers/kernel.h"

int main(int argc, char* argv[]) {

    ultimo_pid=0;

    validar_argumentos_main(argc);

    char* CONFIG_FILE = argv[1];

    logger = iniciar_logger(LOG_FILE, LOG_NAME);
    kernel_config = iniciar_config(CONFIG_FILE);
    communication_config = init_connection_config();

    uint32_t grado_max_multiprogramacion = config_get_int_value(kernel_config,"GRADO_MAX_MULTIPROGRAMACION");
    tiempos_bloqueos = (uint32_t*) config_get_array_value(kernel_config,"TIEMPOS_IO");

    inicializar_mutex();
    inicializar_semaforos_sincronizacion(grado_max_multiprogramacion);
    iniciar_planificacion();

    int socket_srv_kernel = levantar_servidor();
    esperar_modulos(socket_srv_kernel);
    esperar_consolas(socket_srv_kernel);

  }

void validar_argumentos_main(int argumentos){
    uint32_t expected = 2;
    if(argumentos < expected){
		printf(RED"");
		printf("Cantidad de parametros incorrectos.\n");
        printf(" > Expected: %d - Given: %d\n",expected,argumentos);
		printf("   1- Archivo de configuracion\n");
		printf(RESET"");
		exit(argumentos);
	}
}

int levantar_servidor(){
    char* ip_kernel = config_get_string_value(communication_config,"IP_KERNEL");
    char* puerto_kernel = config_get_string_value(communication_config,"PUERTO_KERNEL");

    return iniciar_servidor(ip_kernel, puerto_kernel,logger);
}

void esperar_consolas(int socket_srv){
    log_info(logger,"Esperando consolas..");
    printf("\n");
    while(1){
        pthread_t thread_escucha_consola;
        int socket_consola = esperar_cliente(socket_srv, logger);
        recibir_handshake_inicial(socket_consola,KERNEL,logger);
        pthread_create(&thread_escucha_consola, NULL, conexion_consola, (void*)(intptr_t) socket_consola);
        pthread_detach(thread_escucha_consola);
    }
}

void esperar_modulos(int socket_srv){
    log_info(logger,"Esperando modulos..");
    uint32_t cantidad_modulos = 3; //Dispatch - Interrupt - Memoria
    for(int i=0; i < cantidad_modulos; i++){
        int socket_cliente = esperar_cliente(socket_srv,logger);
        uint32_t modulo = recibir_handshake_inicial(socket_cliente,KERNEL,logger);

        switch(modulo){
            case CPU_DISPATCH:
                pthread_create(&thread_escucha_dispatch, NULL, conexion_dispatch, (void*) (intptr_t)socket_cliente);
                pthread_detach(thread_escucha_dispatch);
                break;
            case CPU_INTERRUPT:
                if(algoritmo_planificacion_tiene_desalojo()){
                    pthread_create(&thread_escucha_interrupt, NULL, conexion_interrupt, (void*) (intptr_t)socket_cliente);
                    pthread_detach(thread_escucha_interrupt);
                }else{
                    close(conexion_interrupt);
                }
                break;
            case MEMORIA:
                pthread_create(&thread_escucha_memoria, NULL, conexion_memoria, (void*) (intptr_t)socket_cliente);
                pthread_detach(thread_escucha_memoria);
                break;
            default:
                log_error(logger,string_from_format("Modulo desconocido actualmente %d",modulo));
                i--;
        }
    }
}

void* conexion_dispatch(void* socket){
    socket_dispatch = (intptr_t) socket;
    t_pcb* pcb;
    while(socket_dispatch != -1){

        sem_wait(&enviar_pcb_a_cpu);
        pcb = quitar_pcb_de_cola(mutex_running,running_queue);
        enviar_PCB(socket_dispatch,pcb,PCB);

        op_code codigo_operacion = recibir_operacion(socket_dispatch);
        pcb = recibir_PCB(socket_dispatch);
        agregar_pcb_a_cola(pcb,mutex_running,running_queue);

        switch(codigo_operacion){
            case BLOQUEAR_PROCESO_IO:      
            case BLOQUEAR_PROCESO_TECLADO:
            case BLOQUEAR_PROCESO_PANTALLA:
            case PAGE_FAULT:
                motivo_bloqueo = codigo_operacion;
                sem_post(&redirigir_proceso_bloqueado);
                break;
            case FINALIZAR_PROCESO:
                break;
            case INTERRUPCION:
                sem_post(&recibi_pcb_por_interrupcion);
                break;
            default:
			    perror("KERNEL -> Conexión Dispatch: Operacion desconocida");
        }
    }
}

void* conexion_interrupt(void* socket){
    int socket_interrupt = (intptr_t) socket;
    uint32_t micro_quantum = quantum * 1000;
    while(socket_interrupt != -1){
        usleep(micro_quantum);
        enviar_interrupcion(socket_interrupt);
        sem_wait(&recibi_pcb_por_interrupcion);
    }
}

void *conexion_memoria(void* socket){
    int socket_memoria = (intptr_t) socket;
    while(socket_memoria != -1){
        op_code codigo_operacion = recibir_operacion(socket_memoria);
    }
}

void *conexion_consola(void* socket){
    printf("\n");
    int socket_consola = (intptr_t) socket;
    while(socket_consola != -1){
        op_code codigo_operacion = recibir_operacion(socket_consola);
        //log_info(logger, string_from_format("Codigo operacion CONSOLA: %d", codigo_operacion));

        switch(codigo_operacion){
            case LISTA_INSTRUCCIONES_SEGMENTOS:
                log_info(logger,"Recibiendo una lista de instrucciones y segmentos");
                t_list* instrucciones = recibir_lista_instrucciones(socket_consola);
                for(int i=0; i<list_size(instrucciones); i++){
                    logear_instruccion(logger,(t_instruccion*) list_get(instrucciones, i));
                }

                t_list* segmentos = recibir_lista_segmentos(socket_consola);
                for(int i=0; i< list_size(segmentos); i++){
                    printf("segmento[%d]: %d\n",i, (uint32_t*) list_get(segmentos, i));
                }

                pthread_mutex_lock(&mutex_pid);
                t_pcb* pcb = crear_estructura_pcb(instrucciones, segmentos);
                pthread_mutex_unlock(&mutex_pid);
                agregar_pcb_a_cola(pcb,mutex_new,new_queue);
                log_info(logger,string_from_format("Se crea el proceso <%d> en NEW",pcb->pid));
                sem_post(&new_to_ready);
                break;
            default:
                break;

        }
    }
}

t_pcb* crear_estructura_pcb(t_list* lista_instrucciones, t_list* segmentos) {
    t_pcb *pcb =  malloc(sizeof(t_pcb));

    pcb->pid = ultimo_pid;
    pcb->registros_pcb.registro_ax=0;
    pcb->registros_pcb.registro_bx=0;
    pcb->registros_pcb.registro_cx=0;
    pcb->registros_pcb.registro_dx=0;
    pcb->datos_segmentos.indice_tabla_paginas_segmentos = 0;
    pcb->datos_segmentos.segmentos = segmentos;
    pcb->lista_instrucciones = lista_instrucciones;
    pcb->program_counter= 0;
    ultimo_pid++;

    return pcb;
}

void iniciar_planificacion(){
    log_info(logger,"Iniciando planificadores..");

    algoritmo_planificacion = config_get_string_value(kernel_config,"ALGORITMO_PLANIFICACION");
    quantum = config_get_int_value(kernel_config,"QUANTUM_RR");

    pthread_t dispatcher;
    pthread_t long_planner;

    new_queue = queue_create();
    exit_queue = queue_create();
    ready1_queue = queue_create();
    if(string_contains(algoritmo_planificacion,"FEEDBACK")){
        ready2_queue = queue_create();
    }
    blocked_screen_queue = queue_create();
    blocked_keyboard_queue = queue_create();
    blocked_page_fault_queue = queue_create();
    blocked_io_queue = queue_create();
    running_queue = queue_create();
    exit_queue = queue_create();

    pthread_create(&long_planner, NULL, planificador_largo_plazo, NULL);
    pthread_detach(long_planner);
    pthread_create(&dispatcher, NULL, planificador_corto_plazo, NULL);
    pthread_detach(dispatcher);
}

void inicializar_mutex(){
    pthread_mutex_init(&mutex_new,NULL);
    pthread_mutex_init(&mutex_ready,NULL);
    pthread_mutex_init(&mutex_running,NULL);
    pthread_mutex_init(&mutex_blocked_screen,NULL);
    pthread_mutex_init(&mutex_blocked_keyboard,NULL);
    pthread_mutex_init(&mutex_blocked_page_fault,NULL);
    pthread_mutex_init(&mutex_blocked_io,NULL);
    pthread_mutex_init(&mutex_exit,NULL);
    pthread_mutex_init(&mutex_dispatch,NULL);
    pthread_mutex_init(&mutex_interrupt,NULL);
    pthread_mutex_init(&mutex_memoria,NULL);
    pthread_mutex_init(&mutex_pid,NULL);
}

void inicializar_semaforos_sincronizacion(uint32_t multiprogramacion){
    sem_init(&grado_multiprogramacion,0,multiprogramacion);
    sem_init(&new_process,0,0);
    sem_init(&new_to_ready,0,0);
    sem_init(&ready_to_running,0,0);
    sem_init(&cpu_libre,0,1);
    sem_init(&finish_process,0,0);
    sem_init(&recibi_pcb_por_interrupcion,0,0);
    sem_init(&enviar_pcb_a_cpu,0,0);
    sem_init(&redirigir_proceso_bloqueado,0,0);
    sem_init(&bloquear_por_io,0,0);
    sem_init(&bloquear_por_pantalla,0,0);
    sem_init(&bloquear_por_teclado,0,0);
    sem_init(&bloquear_por_pf,0,0);
}

void agregar_pcb_a_cola(t_pcb* pcb,pthread_mutex_t mutex, t_queue* cola){
    pthread_mutex_lock(&mutex);
    queue_push(cola,pcb);
    pthread_mutex_unlock(&mutex);
}

t_pcb* quitar_pcb_de_cola(pthread_mutex_t mutex, t_queue* cola){
    pthread_mutex_lock(&mutex);
    t_pcb* pcb = queue_pop(cola);
    pthread_mutex_unlock(&mutex);
    return pcb;
}

int algoritmo_planificacion_tiene_desalojo(){
    return !string_equals_ignore_case(algoritmo_planificacion,"FIFO");
}

void* planificador_largo_plazo(void* x){
    log_info(logger,"Iniciando Planificador Largo plazo..");
    pthread_t process_terminator_thread;
    pthread_create(&process_terminator_thread, NULL, finalizador_procesos, NULL);
    pcb_a_dispatcher();
    pthread_join(process_terminator_thread,NULL);
}

void pcb_a_dispatcher(){
    while(1){
        sem_wait(&new_to_ready);
        sem_wait(&grado_multiprogramacion);
        t_pcb* pcb = quitar_pcb_de_cola(mutex_new,new_queue);
        agregar_pcb_a_cola(pcb,mutex_ready,ready1_queue);
        log_info(logger,string_from_format("PID: <%d> - Estado Anterior <NEW>   - Estado Actual <READY>",pcb->pid));
        sem_post(&ready_to_running);
    }
}

void* finalizador_procesos(void* x){
    while(1){
        sem_wait(&finish_process);
        t_pcb* pcb = quitar_pcb_de_cola(mutex_exit,exit_queue);
        free(pcb);
        //Avisar a memoria
        //Avisar a consola
    }
}

void* planificador_corto_plazo(void* x){
    log_info(logger,"Iniciando Planificador Corto plazo..");
    pthread_t ready_thread;
    pthread_t bloqueador_procesos;
    pthread_t blocked_page_fault_thread;
    pthread_t blocked_io_thread;
    pthread_t blocked_keyboard_thread;
    pthread_t blocked_screen_thread;

    pthread_create(&bloqueador_procesos, NULL, manejador_estado_blocked, NULL);
    pthread_create(&ready_thread, NULL, manejador_estado_ready, NULL);
    pthread_create(&blocked_page_fault_thread, NULL, manejador_estado_blocked_pf, NULL);
    pthread_create(&blocked_io_thread, NULL, manejador_estado_blocked_io, NULL);
    pthread_create(&blocked_screen_thread, NULL, manejador_estado_blocked_screen, NULL);
    pthread_create(&blocked_keyboard_thread, NULL, manejador_estado_blocked_keyboard, NULL);

    pthread_join(ready_thread,NULL);

}

void* manejador_estado_ready(void* x){
    while(1){
        sem_wait(&ready_to_running);
        sem_wait(&cpu_libre);
        t_pcb* pcb = quitar_pcb_de_cola(mutex_ready,ready1_queue);
        agregar_pcb_a_cola(pcb,mutex_running,running_queue);
        log_info(logger,string_from_format("PID: <%d> - Estado Anterior <READY> - Estado Actual <RUNNING>",pcb->pid));
        sem_post(&enviar_pcb_a_cpu);
    }
}

void* manejador_estado_blocked(void* x){
    while(1){
        sem_wait(&redirigir_proceso_bloqueado);
        t_pcb* pcb = quitar_pcb_de_cola(mutex_running,running_queue);
        sem_post(&cpu_libre);

        switch (motivo_bloqueo){
            case BLOQUEAR_PROCESO_IO:
                agregar_pcb_a_cola(pcb,mutex_blocked_io,blocked_io_queue);
                sem_post(&bloquear_por_io);
                break;

            case BLOQUEAR_PROCESO_PANTALLA:
                agregar_pcb_a_cola(pcb,mutex_blocked_screen,blocked_screen_queue);
                sem_post(&bloquear_por_pantalla);
                break;

            case BLOQUEAR_PROCESO_TECLADO:
                agregar_pcb_a_cola(pcb,mutex_blocked_keyboard,blocked_keyboard_queue);
                sem_post(&bloquear_por_teclado);
                break;

            case PAGE_FAULT:
                agregar_pcb_a_cola(pcb,mutex_blocked_page_fault,blocked_page_fault_queue);
                sem_post(&bloquear_por_pf);
                break;
        }
        log_info(logger,string_from_format("PID: <%d> - Estado Anterior <RUNNING> - Estado Actual <BLOCKED>",pcb->pid));
    }
}

void* manejador_estado_blocked_pf(void* x){
    sem_wait(&bloquear_por_pf);

}

void* manejador_estado_blocked_io(void* x){
    uint32_t tiempo_bloqueado;    
    dispositivo io;
    t_pcb* pcb;

    while(1){
        sem_wait(&bloquear_por_io);
        pcb = quitar_pcb_de_cola(mutex_blocked_io,blocked_io_queue);
        obtener_dispositivo_tiempo_bloqueo(pcb,&io,&tiempo_bloqueado);
        log_info(logger,string_from_format("PID: <%d> - Bloqueado por: <%s>",pcb->pid,traducir_dispositivo(io)));
        usleep(tiempo_bloqueado);
        agregar_pcb_a_cola(pcb,mutex_ready,ready1_queue);
    }
}

void* manejador_estado_blocked_screen(void* x){
        sem_wait(&bloquear_por_pantalla);

}

void* manejador_estado_blocked_keyboard(void* x){
        sem_wait(&bloquear_por_teclado);

}

void obtener_dispositivo_tiempo_bloqueo(t_pcb* pcb, dispositivo* disp, uint32_t* tiempo_bloqueo){
    t_instruccion* instruccion_anterior = list_get(pcb->lista_instrucciones,pcb->program_counter-1);
    *disp = instruccion_anterior->parametros[0];
    *tiempo_bloqueo = tiempos_bloqueos[*disp] * instruccion_anterior->parametros[1] * 1000;
}