# Informe de depuración de reinicios y comunicación del sistema I4A

**Destinatario:** tutor del proyecto I4A
**Responsable de las pruebas comunicadas:** Adrián, con aportes de un compañero del proyecto
**Período documentado:** 15–19 de septiembre de 2026
**Fecha de elaboración:** 19 de septiembre de 2026
**Estado:** correcciones implementadas y compiladas; validación funcional y de estabilidad en curso

---

## Resumen ejecutivo

Durante las pruebas de una red de placas ESP32 interconectadas mediante Wi-Fi y un anillo SPI se observaron varios síntomas que inicialmente se presentaban como «reinicios». El análisis permitió separar tres mecanismos distintos:

1. **Desbordamiento de pila de `sys_evt` al obtener una dirección IP.** Se aumentó la pila configurada de esa tarea de 2304 a 8192 bytes en la primera prueba. El fallo no volvió a aparecer en los monitores posteriores compartidos. La configuración actual fue ajustada posteriormente a 6144 bytes; no se dispone de una medición del margen mínimo de pila que permita justificar un tamaño definitivo.
2. **Lectura de un puntero de origen nulo en el hook de enrutamiento IPv4.** Un compañero reprodujo un `LoadProhibited` y aportó una traza que apunta a `src == NULL`. El código leía ese puntero sin validarlo, aunque lwIP puede realizar consultas sin dirección de origen. La versión actual admite ese caso y conserva el enrutamiento personalizado por destino.
3. **Fallo al ejecutar código durante la destrucción de la interfaz AP.** Una placa de orientación 1 produjo `InstrFetchProhibited`, con `PC=0`, al pasar de AP a STA. Se implementó una retirada coordinada del puntero de interfaz en el contexto TCP/IP antes de liberar la interfaz, junto con comprobaciones de disponibilidad en los hooks de rutas. La hipótesis principal es una interacción entre el procesamiento de paquetes y la retirada de la interfaz; la traza no identifica por sí sola el puntero de función exacto que falló.

También se identificaron reinicios solicitados por mensajes del propio anillo, desconexiones Wi-Fi deliberadas por la lógica de provisión, fallos de difusión de RSSI y rechazos de paquetes con `ERR_MEM`. Estos fenómenos deben investigarse por separado y no deben atribuirse automáticamente a falta de pila.

Después del último cambio se comunicaron **31 minutos sin reinicio**. Es un resultado favorable, pero el monitor aportado corresponde a la placa 0 en modo estación: todavía no constituye una demostración de que la placa 1 haya completado varias transiciones AP → STA con tráfico sin fallar.

## 1. Alcance, fuentes y criterios de interpretación

Este informe reconstruye la secuencia de trabajo a partir de los monitores proporcionados, el intercambio de diagnóstico y la inspección del código local. Los fragmentos de log se reproducen de forma selectiva, omitiendo líneas sin relación directa con cada hallazgo.

Las fechas de las secciones siguen el orden de la conversación y los cambios de fecha de la sesión. **Los números entre paréntesis de los logs son tiempos desde el arranque, no fechas de calendario.** La fecha de compilación tampoco identifica necesariamente la hora de ejecución de una prueba. Cuando no existe información suficiente, se conserva el orden relativo sin asignar una hora exacta.

Se distinguen cuatro clases de afirmaciones:

| Clase | Significado en este informe |
|---|---|
| Observación | Aparece directamente en un monitor o en el código inspeccionado. |
| Diagnóstico respaldado | La traza y el código explican el fallo de manera consistente. |
| Hipótesis | Explicación plausible que requiere instrumentación o reproducción adicional. |
| Validación pendiente | Prueba que todavía no fue aportada o completada. |

### Entorno observado

- Hardware: ESP32, revisión de chip v3.1 en los monitores compartidos.
- Entorno local: ESP-IDF v5.5.1, Linux.
- Entorno del compañero: ESP-IDF v5.4.1-dirty, Windows.
- Base del repositorio local al elaborar el informe: `86c7183`, con modificaciones sin confirmar mediante commit.
- El compañero informó una reproducción sobre `upstream/main`, commit `86c7183`, sin los cambios del PR. Esa reproducción se incorpora como evidencia comunicada; no fue repetida independientemente en esta sesión.

## 2. Conceptos necesarios para interpretar los fallos

### 2.1. Pila de una tarea

Cada tarea utiliza una región de RAM denominada **pila** para variables locales y llamadas a funciones. Si las llamadas y sus datos superan el espacio reservado, puede producirse un desbordamiento. Aumentar esa región puede resolver un desbordamiento real, pero no corrige accesos a punteros inválidos ni órdenes explícitas de reinicio.

En ESP-IDF los tamaños de pila de las APIs relevantes se expresan en bytes. La opción de configuración de una tarea interna y la asignación efectiva pueden diferir por márgenes adicionales del framework; por eso se informa el valor configurado, sin presentarlo como una medición del consumo real.

### 2.2. Interfaz y puntero

Una interfaz de red es una estructura que representa una vía de entrada o salida de paquetes. I4A utiliza Wi-Fi y SPI. ESP-IDF expone un `esp_netif_t`; lwIP utiliza internamente un `struct netif`.

Un puntero conserva la dirección en memoria de una estructura. No mantiene automáticamente viva esa estructura. Después de liberarla, conservar su dirección y usarla puede provocar un acceso inválido.

`NULL` significa ausencia de una referencia. Debe distinguirse de una dirección IPv4 cuyo valor numérico es cero: una cosa es no recibir un dato y otra recibir el valor `0.0.0.0`.

### 2.3. AP, STA, provisión y enrutamiento

- **AP:** la placa crea un punto de acceso Wi-Fi.
- **STA:** la placa se conecta a un punto de acceso existente.
- **Provisión:** el protocolo entrega al nodo parámetros de red y rutas para su funcionamiento.
- **Hook de enrutamiento:** función del proyecto a la que lwIP consulta qué interfaz debe utilizar para un destino.
- **Contexto TCP/IP:** tarea que procesa las operaciones de la pila de red en la configuración utilizada.

## 3. Cronología de las pruebas y decisiones

### 3.1. Primer fallo: desbordamiento de `sys_evt` al obtener IP — 15 de septiembre

El primer monitor llegaba a la conexión Wi-Fi y a la configuración de IP. A continuación se detenía con:

```text
I (77662) esp_netif_handlers: sta ip: 10.32.0.2, mask: 255.255.255.252, gw: 10.32.0.2
I (77662) esp_netif_handlers: sta ip: 10.32.0.2, mask: 255.255.255.252, gw: 10.32.0.1

***ERROR*** A stack overflow in task sys_evt has been detected.
...
vApplicationStackOverflowHook
...
Rebooting...
```

**Inspección realizada.** Se revisó el evento `IP_EVENT_STA_GOT_IP` en `components/wireless/station/station.c`. Durante ese evento, la aplicación puede ejecutar `cm_provide_to_siblings()`, iniciar el cliente de información e iniciar el cliente TCP.

El envío de canales recorre, entre otras funciones:

```text
event_handler
  → cm_provide_to_siblings
  → rs_broadcast
  → sb_broadcast_to_siblings
  → node_broadcast_to_siblings
  → broadcast_to_siblings / send_broadcast
```

En `components/internal/ring_link_internal/broadcast.c`, `send_broadcast()` declara un `ring_link_payload_t` local. La configuración SPI observada es de 1600 bytes. Ese paquete ocupa aproximadamente ese orden de espacio en pila, al que se agregan las demás llamadas.

**Interpretación.** El desbordamiento está confirmado por el monitor. El recorrido de difusión es una causa probable de presión de pila, pero no se capturó una medición por función ni el máximo de consumo.

**Cambio aplicado en la primera prueba:**

```diff
-CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304
+CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=8192
```

Se actualizó `app/sdkconfig`, incluido su alias de compatibilidad, y se agregó la opción a `components/sdkconfig.defaults` con un comentario sobre el envío de paquetes desde los manejadores Wi-Fi/IP.

**Verificación:** `idf.py build` terminó correctamente. Se indicó que `monitor` por sí solo no instala cambios: es necesario flashear el firmware nuevo.

**Resultado posterior:** los monitores siguientes superaron la obtención de IP, difundieron canales y establecieron TCP sin repetir ese desbordamiento.

**Estado actual:** `components/sdkconfig.defaults` y `app/sdkconfig` contienen 6144 para esa opción. El ajuste intermedio de 8192 a 6144 no fue documentado como una prueba controlada en la conversación. No debe confundirse la primera intervención con la configuración actual.

### 3.2. La placa continúa, pero recibe órdenes de reinicio

El monitor posterior mostró progreso real respecto del primer fallo:

```text
I (77562) channel_manager: Provided channels to siblings, connected channel=1, ssid=I4A_N_000000000000
I (77562) info_manager: Info manager UDP client started
I (77562) tcp_client: Client started
I (77592) tcp_client: Connected to 10.32.0.1
I (377562) info_manager: UDP socket created
I (377562) info_manager: UDP packet sent (537 bytes)
```

En una ejecución posterior del mismo monitor:

```text
W (680182) reset_manager: Reset signal received: resetting device
I (682182) wifi:state: run -> init (0x0)
...
rst:0xc (SW_CPU_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
```

Se inspeccionó `components/integration/reset_manager/src/reset_manager.c`. Al recibir `RM_OPCODE_RESET`, si la placa ya está operativa, el código espera `RESET_BROADCAST_WAIT_MS`, configurado en 2000 ms, y llama a `esp_restart()`.

**Conclusión:** esos reinicios concretos fueron solicitados por la lógica del anillo. No presentaban el desbordamiento anterior. El log receptor no identifica por sí solo la placa emisora ni el motivo original.

También se inspeccionó `node_setup()` en `components/integration/node/node.c`: el central transmite un reset durante su inicialización y una placa no central que permanece esperando el arranque puede transmitirlo al vencer el tiempo previsto. Esto proporciona mecanismos posibles para una propagación de reinicios, sin demostrar cuál actuó en cada monitor.

**Acción:** se solicitó contrastar logs de otras placas en el mismo intervalo. No se deshabilitó el protocolo de reset.

### 3.3. Fallos intermitentes de difusión de RSSI

Otros monitores mostraron:

```text
E (68702) priority_manager: Failed to broadcast RSSI: orientation=0, rssi=-1
...
I (87642) tcp_client: Connected to 10.32.0.1
```

En otra prueba se recibían repetidamente RSSI de las orientaciones 1 y 3, sin aparecer el de la orientación 2:

```text
I (64672) priority_manager: Stored RSSI in priority list: orientation=1, rssi=-33
I (66562) priority_manager: Stored RSSI in priority list: orientation=3, rssi=-67
E (68712) priority_manager: Failed to broadcast RSSI: orientation=0, rssi=0
```

**Interpretación:** no se completó satisfactoriamente la difusión por el anillo. Puede fallar la transmisión o su confirmación; ese mensaje no demuestra que todas las placas hayan dejado de recibir. La ausencia de la orientación 2 era una pista para revisar sus enlaces, no una identificación definitiva de una placa defectuosa.

En pruebas posteriores sí se registraron las cuatro orientaciones y la difusión de canales. Por tanto, no se caracterizó el problema como una interrupción permanente del anillo.

**Acción:** inspección del recorrido de difusión y recomendación de obtener logs simultáneos. No se aplicó una modificación específica a este mecanismo.

### 3.4. Desconexión Wi-Fi al recibir información de canales

Un monitor mostró una conexión TCP establecida y después:

```text
I (115632) channel_manager: Stored network in block list: orientation=3, ssid=000000000000
I (115632) wifi:state: run -> init (0x0)
W (115652) tcp_client: Closing client...
E (115652) station: Failed to connect, disconnecting
I (115652) channel_manager: Updated suggested channel=5
E (115662) tcp_client: Receive error from 10.32.0.1: errno 113
W (120672) tcp_client: STA not connected, stopping task...
```

En `components/integration/channel_manager/src/channel_manager.c`, el receptor del mensaje guarda el identificador de red bloqueado y ejecuta:

```c
if (!node_is_network_provided()) {
    node_disable_sta();
    cm->suggested_channel = packet->channels[cm->orientation];
}
```

**Conclusión:** la lógica de descubrimiento/provisión desactiva STA para evitar conexiones múltiples en esa etapa. El error TCP ocurre después de la desconexión. El texto `Failed to connect` es engañoso para esta secuencia porque el manejador también lo imprime tras una desconexión solicitada por el propio programa.

**Pendiente:** determinar si el estado de provisión y el momento de esa desactivación son correctos para el protocolo, y explicar la repetición de mensajes. No se eliminó la desactivación ni se cambió la política de conexión.

### 3.5. UDP sigue enviando y aparecen avisos de Wi-Fi

La continuación mostró:

```text
W (295992) wifi:Haven't to connect to a suitable AP now!
I (387672) info_manager: UDP socket created
I (387672) info_manager: UDP packet sent (424 bytes)
W (420992) wifi:Haven't to connect to a suitable AP now!
I (507672) info_manager: UDP packet sent (424 bytes)
```

Se revisó `im_client_task()` en `components/integration/info_manager/src/info_manager.c`. La tarea continúa su bucle y llama a `sendto()` sin comprobar previamente que STA esté conectada.

**Alcance de la evidencia:** `UDP packet sent` se imprime cuando la llamada acepta los bytes. No demuestra recepción en el servidor, recuperación de Wi-Fi ni el camino físico utilizado. El sistema dispone también de interfaces SPI, por lo que el éxito local de la llamada no debe interpretarse como una prueba de conectividad Wi-Fi.

No se modificó el cliente UDP como parte de las correcciones de reinicio.

### 3.6. Errores de entrada IP: `netif->input error: -1`

En una prueba con conexión Wi-Fi y TCP establecida aparecieron ráfagas:

```text
I (77722) tcp_client: Connected to 10.32.0.1
I (377692) info_manager: UDP packet sent (518 bytes)
E (516612) ring_link: netif->input error: -1
E (516622) ring_link: netif->input error: -1
E (542472) ring_link: netif->input error: -1
I (617692) info_manager: UDP packet sent (533 bytes)
```

El mensaje se origina en `components/internal/ring_link_netif/ring_link_netif_rx.c`, después de:

```c
result = netif->input(p, netif);
```

En lwIP, `-1` corresponde a `ERR_MEM`. En el recorrido `tcpip_inpkt()` inspeccionado en ESP-IDF, ese valor puede producirse al no poder reservar un mensaje interno o al no poder colocarlo en la cola TCP/IP.

**Conclusión:** algunos paquetes recibidos por el anillo no fueron aceptados para su procesamiento IP. No se determinó si el recurso limitante era memoria o capacidad de cola. No es evidencia de desbordamiento de pila.

**Próximas comprobaciones propuestas, todavía no implementadas:** medir memoria libre, contar rechazos y agrupar los mensajes para evitar sobrecargar el monitor; revisar el ciclo de propiedad y liberación del búfer.

**Precisión tras la revisión del código:** el receptor ya contiene un `pbuf_free(q)` condicionado a que `esp_netif_receive()` devuelva error. No se confirmó una fuga. Debe verificarse que el error se propague en la configuración efectiva y que el búfer se libere exactamente una vez. Agregar otro `pbuf_free()` sin revisar ese contrato podría introducir una doble liberación.

### 3.7. Interpretación de `rssi=-128`

```text
I (653572) priority_manager: Stored RSSI in priority list: orientation=1, rssi=-128
I (656592) priority_manager: Stored RSSI in priority list: orientation=2, rssi=-128
I (660622) priority_manager: Stored RSSI in priority list: orientation=3, rssi=-128
```

En `station_scan_best_rssi()`, archivo `components/wireless/station/station.c`, el mejor valor se inicializa en `-128` y solo cambia cuando se encuentra una red permitida. Además, no se comprueba el retorno de la llamada que inicia el escaneo.

**Interpretación:** puede significar que no se encontró un AP permitido, que todas las redes fueron filtradas o que el escaneo no produjo resultados utilizables. No debe interpretarse necesariamente como una medición de −128 dBm ni como prueba de reinicio de las placas emisoras.

La variación del tamaño de los mensajes UDP, por ejemplo de 521 a 492 bytes, indica variación del contenido serializado; no demuestra por sí sola un fallo.

### 3.8. Persistencia de la configuración de flash de 4 MB

Los primeros arranques advertían:

```text
W (...) spi_flash: Detected size(4096k) larger than the size in the binary image header(2048k).
```

Se solicitó que el repositorio conservara la capacidad correcta sin tener que usar `menuconfig` en cada instalación.

Se agregó a `components/sdkconfig.defaults`:

```ini
# Board SPI flash capacity.
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
```

`app/CMakeLists.txt` ya seleccionaba ese archivo:

```cmake
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/../components/sdkconfig.defaults")
```

`app/sdkconfig` se genera localmente. Los valores por defecto se aplican al crear una configuración; no sustituyen necesariamente opciones previamente guardadas en un `sdkconfig` existente. Al realizar esta intervención, la configuración local ya indicaba 4 MB. Se ejecutó `idf.py reconfigure` y se verificó el valor generado.

Los monitores posteriores confirmaron:

```text
I (41) boot.esp32: SPI Flash Size : 4MB
```

No se amplió la partición de aplicación: la tabla mostrada conserva una partición `factory` de 1 MiB. Configurar 4 MB físicos no redistribuye automáticamente las particiones.

La advertencia sobre el chip BOYA y el controlador genérico es independiente. No se cambió ese controlador ni se demostró que fuera causa de los reinicios.

### 3.9. Consulta sobre logging y rendimiento

Se explicó que cerrar el monitor del ordenador no desactiva los logs del firmware: los mensajes habilitados continúan formateándose y enviándose. Esto consume tiempo de CPU, pila y capacidad de salida serie; según el recorrido de escritura, puede introducir espera.

Se recomendó reducir el nivel general durante pruebas de rendimiento y habilitar detalle solo en el componente investigado. No se midió el impacto ni se atribuyeron los fallos exclusivamente al logging. Tampoco se implementó todavía la agrupación de errores repetidos.

### 3.10. Prueba de aumento de pila de `node_ap_sta_cycle` — desde el 16 de septiembre

Se localizó en `components/integration/task_config/task_config.h`:

```c
#define TASK_NODE_AP_STA_STACK       4096
#define TASK_NODE_AP_STA_PRIORITY    LOW_PRIORITY
```

En la primera inspección de esa consulta, la llamada a `xTaskCreatePinnedToCore()` estaba comentada en `node.c`; por tanto, cambiar esa constante no tenía efecto sobre una tarea no creada. En una inspección posterior la tarea ya estaba habilitada. Este cambio de estado del código se informó expresamente.

Se planteó que 6144 u 8192 bytes podían utilizarse como prueba si la tarea estaba activa, pero que el dimensionamiento debía basarse en una medición de pila. Se desaconsejó aumentar la prioridad para resolver errores de memoria.

El usuario comunicó que duplicar la memoria no resolvió el problema. El diff actual contiene:

```diff
-#define TASK_NODE_AP_STA_STACK       4096
+#define TASK_NODE_AP_STA_STACK       8192
```

La prioridad permanece en `LOW_PRIORITY`. Este aumento es una prueba separada de la posterior corrección de interfaces. No hay evidencia de que fuera necesario para resolver los fallos de punteros.

### 3.11. Aporte del compañero: `LoadProhibited` por origen nulo

El compañero informó una reproducción sobre la base del proyecto, con ESP-IDF v5.4.1:

```text
W (53665) home: [on_provision] Got subnet 0A940000/14
I (53675) home: [on_provision] Added entry to node routing table
Guru Meditation Error: Core 0 panic'ed (LoadProhibited).
PC : 0x400d90df
--- custom_ip4_route_src_hook at .../app/main/main.c:30
A2 : 0x00000000
EXCVADDR: 0x00000000
```

El backtrace incluía:

```text
custom_ip4_route_src_hook
ip4_route
ip4_route_src
ip4_forward
ip4_input
tcpip_thread_handle_msg
tcpip_thread
```

El código original de `app/main/main.c` era:

```c
struct netif *custom_ip4_route_src_hook(const ip4_addr_t *src,
                                      const ip4_addr_t *dest) {
    uint32_t src_ip = lwip_ntohl(ip4_addr_get_u32(src));
    uint32_t dst_ip = lwip_ntohl(ip4_addr_get_u32(dest));
    return node_do_routing(src_ip, dst_ip);
}
```

Se confirmó en la implementación local de lwIP que `ip4_route()` puede invocar:

```c
LWIP_HOOK_IP4_ROUTE_SRC(NULL, dest);
```

**Diagnóstico respaldado:** la función leía `src` cuando era nulo. Aumentar pila, cambiar prioridad o modificar TTL no corrige esa lectura.

#### Primera propuesta y revisión posterior

Inicialmente se propuso salir del hook si faltaba el origen:

```c
if (src == NULL) {
    return NULL;
}
```

Esto evita el acceso inválido, pero permite que lwIP busque una ruta normal, en lugar de aplicar las reglas personalizadas. Se señaló desde esa propuesta que era necesario validar el efecto sobre el anillo.

Más adelante se evaluó una alternativa aportada por el usuario que convertía tanto el origen como el destino ausentes en cero. La inspección de `rt_do_route()` mostró que el origen se ignora explícitamente con `(void)src_ip`; los hooks incorporados deciden por destino.

Por ello se recomendó conservar la selección personalizada cuando solo falta el origen, pero no inventar un destino cuando este no existe. La versión actual es:

```c
struct netif *custom_ip4_route_src_hook(
    const ip4_addr_t *src, const ip4_addr_t *dest)
{
    if (dest == NULL) {
        return NULL;
    }

    // lwIP puede consultar una ruta sin proporcionar el origen.
    uint32_t src_ip = src ? lwip_ntohl(ip4_addr_get_u32(src)) : 0;
    uint32_t dst_ip = lwip_ntohl(ip4_addr_get_u32(dest));

    return node_do_routing(src_ip, dst_ip);
}
```

| Caso | Resultado de la versión actual |
|---|---|
| Origen y destino presentes | Convierte ambos valores y llama al enrutamiento personalizado. |
| Origen ausente, destino presente | Usa origen cero y conserva la decisión por destino. |
| Destino ausente | Devuelve `NULL` sin intentar enrutar hacia `0.0.0.0`. |

Esta solución depende de que cero sea aceptable como origen no especificado. Si se incorpora un hook personalizado que use el origen, deberá respetar ese significado. Las comprobaciones no agregan esperas ni transmisiones y su costo esperado es pequeño; no se realizó un benchmark.

### 3.12. Monitor del central: reinicio por orden después de unos 31 minutos

Otro monitor del central mostró provisión completa y servicio AP:

```text
W (53730) home: [on_provision] Got subnet 0A340000/14
I (53750) home: [on_provision] Added entry to node routing table
I (57870) home: [on_provision] Enabled AP mode
I (214300) esp_netif_lwip: DHCP server assigned IP to a client, IP is: 10.52.0.2
```

Más adelante, una línea parcialmente dañada conservaba el texto:

```text
... signal received: resetting device
I (1883250) wifi:station: ... leave ...
...
rst:0xc (SW_CPU_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
```

No había un panic ni un backtrace. La secuencia coincide con el manejador de reset del anillo. El hecho de que el central haya funcionado alrededor de 31 minutos antes de reiniciar no implica que se haya agotado su memoria.

**Pendiente:** localizar el emisor de la orden mediante monitores correlacionados. Una placa que reinicie o quede en inicialización es una posibilidad, no una causa demostrada para este episodio.

### 3.13. Nuevo fallo: `InstrFetchProhibited` al destruir AP — 18 de septiembre

La placa de orientación 1 recibió provisión, pasó a AP y diez minutos después intentó cambiar de modo:

```text
I (80320) forwarder: [on_provision] Provisioned: 0A200000/11 by 1 [dtr=2]
I (81360) AP: Starting AP
I (81370) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF with IP: 10.40.0.1
...
I (681340) AP: Stopping AP
W (681350) AP: Destroying AP netif...
I (681350) node_traffic: Stopping traffic monitoring on netif 0x3ffafbe8
Guru Meditation Error: Core  0 panic'ed (InstrFetchProhibited).
PC      : 0x00000000
EXCVADDR: 0x00000000
```

El backtrace señalaba `ip4_input`, `tcpip_thread_handle_msg` y `tcpip_thread`. El intervalo coincide con la espera de diez minutos de `node_ap_sta_cycle_task()`.

**Diferencia con el fallo anterior:** `LoadProhibited` indicaba una lectura inválida; `InstrFetchProhibited` con `PC=0` es compatible con un intento de ejecutar una función a través de una dirección nula. No se identificó mediante desensamblado el puntero exacto. Se consideró como hipótesis principal el uso de una interfaz durante su retirada o destrucción.

**Acción autorizada e implementada:** coordinar la retirada de la referencia desde TCP/IP y validar las interfaces elegidas. El detalle completo está en la sección 4.

### 3.14. Resultado de compilación y monitores posteriores — 18–19 de septiembre

Se compiló la corrección con `idf.py -C app build`. La compilación terminó correctamente. También se verificó el formato del diff de los tres archivos modificados para esta intervención.

El nuevo monitor correspondía a la placa 0:

```text
I (530) ==> config: Board ID: '0'
I (540) ==> config: Board Orientation: '0'
I (44570) tcp_client: Connected to 10.32.0.1
E (152050) ring_link: netif->input error: -1
...
I (704590) info_manager: UDP packet sent (507 bytes)
```

Las continuaciones alcanzaron más de 17 minutos sin panic visible, aunque seguían los errores de entrada IP. Luego el usuario informó 31 minutos sin reinicio.

**Resultado:** evidencia favorable de continuidad en esa ejecución. **Límite:** no aparece la destrucción de AP de la placa 1 que originaba el fallo. Es necesario registrar esa transición, no únicamente superar diez o treinta minutos de funcionamiento en una placa STA.

## 4. Detalle de la corrección de transición AP ↔ STA

Esta sección documenta los tres archivos modificados específicamente para el último arreglo. Los cambios de pila, flash y `main.c` son intervenciones distintas.

### 4.1. Problema del orden anterior

Antes, el destructor ejecutaba conceptualmente:

```text
Detener monitoreo → desregistrar eventos → destruir interfaz → borrar referencia
```

La aplicación podía conservar temporalmente una referencia a un objeto ya liberado. Por otra parte, los hooks devolvían directamente la interfaz interna sin verificar si estaba habilitada o tenía función de salida.

Una intercalación peligrosa sería:

| Paso | Tarea de cambio de modo | Tarea TCP/IP |
|---|---|---|
| 1 | Destruye la interfaz | — |
| 2 | Aún conserva el puntero publicado | Consulta el puntero viejo |
| 3 | — | Intenta utilizar la interfaz |
| 4 | Pone el puntero en `NULL` | Ya pudo ocurrir el acceso inválido |

Es un ejemplo que explica la ventana de riesgo, no una reconstrucción instrucción por instrucción de la ejecución fallida.

### 4.2. Archivo `components/wireless/access_point/access_point.c`

#### Include agregado

```c
#include "lwip/tcpip.h"
```

Permite utilizar la API de callbacks de TCP/IP.

#### Función agregada

```c
// Runs in TCP/IP context, after any packet currently using this interface.
static void ap_detach_routing_netif(void *ctx) {
  esp_netif_t **slot = (esp_netif_t **)ctx;
  *slot = NULL;
}
```

El argumento apunta al lugar donde se guarda `ap->netif`. El doble puntero permite modificar ese campo. `*slot = NULL` retira la referencia compartida: no libera ni copia la interfaz.

#### Bloque anterior de `ap_destroy_netif()`

```c
node_traffic_stop(ap->netif);
esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                             &ap_event_handler);
esp_netif_destroy_default_wifi(ap->netif);
ap->netif = NULL;
```

#### Bloque nuevo

```c
esp_netif_t *netif = ap->netif;
// Withdraw from custom routing before ESP-IDF frees the interface.
// Never call this destructor from the TCP/IP task itself.
ESP_ERROR_CHECK(
    tcpip_callback_wait(ap_detach_routing_netif, &ap->netif) == ERR_OK
        ? ESP_OK : ESP_FAIL);
node_traffic_stop(netif);
esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                             &ap_event_handler);
esp_netif_destroy_default_wifi(netif);
```

La variable local `netif` conserva la dirección para completar la limpieza. No reserva una segunda interfaz. El campo compartido se retira antes y el destructor usa la copia local después.

Se eliminó la asignación final `ap->netif = NULL`, porque ahora se realiza en el callback previo a la destrucción. Las comprobaciones exteriores de existencia de la interfaz y los logs del destructor se mantuvieron.

### 4.3. Archivo `components/wireless/station/station.c`

Se agregó también:

```c
#include "lwip/tcpip.h"
```

Y el callback equivalente:

```c
// Runs in TCP/IP context, after any packet currently using this interface.
static void station_detach_routing_netif(void *ctx) {
  esp_netif_t **slot = (esp_netif_t **)ctx;
  *slot = NULL;
}
```

#### Bloque anterior de `station_destroy_netif()`

```c
node_traffic_stop(stationPtr->netif);
esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                             &event_handler);
esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                             &event_handler);
esp_netif_destroy_default_wifi(stationPtr->netif);
stationPtr->netif = NULL;
```

#### Bloque nuevo

```c
esp_netif_t *netif = stationPtr->netif;
// Withdraw from custom routing before ESP-IDF frees the interface.
// Never call this destructor from the TCP/IP task itself.
ESP_ERROR_CHECK(
    tcpip_callback_wait(station_detach_routing_netif,
                        &stationPtr->netif) == ERR_OK
        ? ESP_OK : ESP_FAIL);
node_traffic_stop(netif);
esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                             &event_handler);
esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                             &event_handler);
esp_netif_destroy_default_wifi(netif);
```

La protección se aplica a ambos sentidos de la transición: también se destruye STA al volver al modo AP.

### 4.4. Por qué se usa `tcpip_callback_wait()`

En la configuración observada, `CONFIG_LWIP_TCPIP_CORE_LOCKING` no está habilitada. La operación solicita ejecutar el callback en la tarea TCP/IP y espera su finalización.

El orden buscado es:

```text
Tarea de cambio de modo                 Tarea TCP/IP
----------------------                 ------------
Guarda referencia local
Solicita retirar el puntero
Espera                                 Completa el procesamiento en curso
                                       Ejecuta el callback de retirada
                                       Pone el puntero compartido en NULL
Continúa
Limpia y destruye la interfaz
```

Esto coordina la retirada con las consultas de rutas realizadas en ese contexto. Poner `NULL` desde otra tarea, sin coordinación, no impide que TCP/IP ya haya obtenido la referencia inmediatamente antes.

**La operación no vacía todas las colas ni detiene permanentemente la red.** Crea un punto de orden para retirar la referencia utilizada por el enrutamiento personalizado. Tampoco equivale a una garantía general para cualquier otro consumidor de punteros desde otras tareas.

### 4.5. Archivo `components/integration/routing_hooks/routing_hooks.c`

#### Include agregado

```c
#include "lwip/netif.h"
```

#### Helper agregado

```c
static struct netif *ready_netif(esp_netif_t *handle) {
    if (!handle) {
        return NULL;
    }
    struct netif *netif = esp_netif_get_netif_impl(handle);
    if (!netif || !netif_is_up(netif) || !netif->output) {
        return NULL;
    }
    return netif;
}
```

| Condición | Qué evita |
|---|---|
| `!handle` | Consultar una interfaz no publicada o retirada. |
| `!netif` | Entregar una implementación interna ausente. |
| `!netif_is_up(netif)` | Seleccionar una interfaz administrativamente apagada. |
| `!netif->output` | Entregar una interfaz sin función de salida asignada. |

Estas comprobaciones no detectan por sí solas un puntero arbitrario ya liberado. Su seguridad depende también de la coordinación de retirada. Tampoco prueban que exista asociación Wi-Fi, enlace físico o recepción remota.

#### Reemplazos efectuados

En `routing_hook_root_center()`, `routing_hook_forwarder()` y `routing_hook_home()`, se reemplazaron las conversiones directas:

```c
return (struct netif *)esp_netif_get_netif_impl(node_get_wifi_netif());
return (struct netif *)esp_netif_get_netif_impl(node_get_spi_netif());
```

por las correspondientes llamadas validadas:

```c
return ready_netif(node_get_wifi_netif());
return ready_netif(node_get_spi_netif());
```

En `routing_hook_default()`:

```diff
-struct netif *lwip_netif = esp_netif_get_netif_impl(spi);
+struct netif *lwip_netif = ready_netif(spi);
```

Se conservaron las reglas de selección por destino o tabla de rutas. La validación se aplica después de elegir el tipo de salida. El mecanismo de hooks personalizados registrados por terceros no se transformó en un sistema de validación universal.

### 4.6. Significado de devolver `NULL`

Un `NULL` devuelto por el hook significa que este no seleccionó una interfaz. lwIP puede continuar con su búsqueda normal. **No significa necesariamente descartar el paquete, enviarlo por SPI o detener todo el tráfico.**

Este detalle exige validar el comportamiento de las rutas durante el intervalo en el que una interfaz se retira y la siguiente todavía no está disponible. Evitar un acceso inválido y conservar la política de enrutamiento son objetivos relacionados, pero diferentes.

### 4.7. Costo y restricciones

- El helper introduce comprobaciones cortas por consulta. No contiene esperas, reservas de memoria ni transmisiones adicionales. No se cuantificó su costo mediante benchmark.
- La coordinación puede hacer esperar a la tarea que destruye la interfaz hasta que TCP/IP atienda el callback. Ocurre durante la transición, no en cada paquete, y no es un retardo fijo.
- Los destructores no deben ejecutarse desde la propia tarea TCP/IP bajo esta configuración: podría esperarse un trabajo que necesita ejecutar la misma tarea. El comentario del código documenta esa restricción, pero no es una comprobación automática del contexto.
- El resultado se verifica con `ESP_ERROR_CHECK()`. Si la coordinación falla, se aborta en lugar de continuar con una liberación no coordinada. Por tanto, el cambio no promete eliminar cualquier posible reinicio.
- Las transiciones deben permanecer controladas: el cambio no introduce un bloqueo general contra múltiples destructores o inicializadores concurrentes.

## 5. Inventario y separación de cambios

| Archivo | Intervención documentada | Estado o alcance |
|---|---|---|
| `components/sdkconfig.defaults` | Pila de eventos y flash de 4 MB | Primera prueba de pila: 8192; valor actual: 6144. |
| `app/sdkconfig` | Configuración local generada | No requiere versionarse; los defaults no sobrescriben automáticamente valores existentes. |
| `app/main/main.c` | Validación de origen y destino en el hook | Versión actual conserva enrutamiento por destino con origen ausente. |
| `components/integration/task_config/task_config.h` | Prueba de pila AP/STA de 4096 a 8192 | No se demostró que resuelva los fallos; prioridad sin cambio. |
| `components/wireless/access_point/access_point.c` | Retirada coordinada de AP | Parte del último arreglo. |
| `components/wireless/station/station.c` | Retirada coordinada de STA | Parte del último arreglo. |
| `components/integration/routing_hooks/routing_hooks.c` | Validación de interfaces seleccionadas | Parte del último arreglo. |
| `components/integration/node/node.c` | Se observaron cambios de estado de la tarea y configuración de SSID | No se atribuyen al último arreglo de destrucción. |
| `components/integration/info_manager/src/info_manager.c` | Modificaciones preexistentes en el árbol de trabajo | No se atribuyen a las correcciones de reinicio descritas. |

La presencia de un archivo modificado en `git diff` no demuestra que todo su contenido haya sido cambiado durante una misma intervención. Los tres archivos del último arreglo deben revisarse separadamente de los cambios previos. No se realizó un push a GitHub durante las intervenciones documentadas.

## 6. Resultados, límites y plan de validación

| Problema | Evidencia alcanzada | Qué falta |
|---|---|---|
| Desbordamiento de `sys_evt` | No se repite en los monitores posteriores al aumento inicial. | Medir margen mínimo de pila con la configuración actual. |
| Lectura de `src == NULL` | Traza y código respaldan el diagnóstico; guardas presentes en la versión actual. | Verificar provisión y rutas con origen ausente en todas las funciones utilizadas. |
| Fallo al destruir AP | Corrección compilada; continuidad posterior comunicada. | Repetir AP → STA → AP bajo tráfico en la placa que fallaba. |
| Reinicio por protocolo | Mensajes de reset y código receptor explican la acción. | Identificar emisor y causa inicial mediante logs simultáneos. |
| `ERR_MEM` en recepción | Rechazos visibles y sostenidos con la aplicación en ejecución. | Medir recursos, revisar cola y propiedad de búferes. |
| Difusión de RSSI | Fallos intermitentes y pruebas exitosas. | Correlacionar envío, confirmación y recepción entre placas. |
| Caracteres ilegibles | Presentes en varios monitores. | Determinar su origen; no atribuirlos sin prueba a corrupción de memoria. |

### Protocolo recomendado para la siguiente prueba

1. Registrar por placa: ID, orientación, versión de ESP-IDF y SHA del ELF. Confirmar que se cargó el binario construido y no solo se abrió el monitor.
2. Capturar la placa 1 que fallaba, junto con el central y, si es posible, las demás placas. Los tiempos relativos a cada arranque requieren correlación externa.
3. Mantener tráfico durante varias transiciones. Registrar `Stopping AP`, `Destroying AP netif`, inicio de STA y retorno a AP. Superar un tiempo de actividad sin ejecutar ese recorrido no valida el arreglo.
4. Comprobar recepción real en el extremo remoto, rutas elegidas y recuperación de la comunicación. El éxito de `sendto()` no basta.
5. Medir el mínimo de pila libre de las tareas relevantes mediante `uxTaskGetStackHighWaterMark()`, bajo carga representativa, antes de fijar tamaños definitivos.
6. Para `ERR_MEM`, incorporar contadores y memoria libre con reportes periódicos; revisar la liberación y propagación de errores antes de modificar tamaños de colas.
7. Si vuelve a aparecer `Reset signal received`, identificar quién lo emitió. Si aparece un panic, conservar la traza completa y el ELF correspondiente.

## 7. Conclusión para la revisión del tutor

La depuración avanzó desde una explicación general basada en memoria hacia la separación de fallos de pila, accesos nulos, ciclo de vida de interfaces y decisiones del protocolo. Esa separación evitó tratar todos los reinicios mediante aumentos de pila o prioridad.

La última intervención corrige un orden de destrucción riesgoso: la referencia compartida se retira en el contexto TCP/IP antes de liberar la interfaz, y los hooks incorporados comprueban disponibilidad antes de devolverla. La compilación fue exitosa y se comunicó una ejecución prolongada sin reinicio, pero todavía no se cuenta con una validación completa de transiciones repetidas en la placa afectada.

El resultado debe presentarse como **una corrección implementada con evidencia preliminar favorable**, no como una demostración de estabilidad total. La investigación de `ERR_MEM` y de las órdenes de reset del anillo sigue abierta.

## Referencias técnicas

- [ESP-IDF: FreeRTOS, versión 5.5](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/system/freertos_idf.html): APIs de tareas y medición del margen de pila.
- [ESP-IDF: errores fatales](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32/api-guides/fatal-errors.html): interpretación de excepciones como `LoadProhibited` e `InstrFetchProhibited`.
- [lwIP: hooks](https://www.nongnu.org/lwip/2_1_x/group__lwip__opts__hooks.html): contrato de retorno del hook de rutas.
- [lwIP: hooks, documentación 2.0](https://www.nongnu.org/lwip/2_0_x/group__lwip__opts__hooks.html): origen potencialmente nulo en consultas de enrutamiento.
- [lwIP: códigos de error](https://www.nongnu.org/lwip/2_1_x/group__infrastructure__errors.html): `ERR_MEM`.
- Código local inspeccionado: `esp-idf/components/lwip/lwip/src/core/ipv4/ip4.c`, `src/api/tcpip.c` y `esp-idf/components/esp_netif/lwip/esp_netif_lwip.c`. El comportamiento concreto se contrastó con esta implementación; las referencias web no sustituyen la revisión de la versión compilada.
