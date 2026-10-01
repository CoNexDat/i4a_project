# Arquitectura de software

El proyecto ESP-IDF vive en `app/` e importa cuatro familias de componentes:

- `internal`: configuración de pines, SPI, anillo y adaptación a lwIP.
- `wireless`: WiFi AP/STA y cliente/servidor TCP entre nodos.
- `routing`: roles root/home/forwarder, tablas, token ring y estado compartido.
- `integration`: arranque, callbacks, telemetría, canales, reset, control remoto y OTA.

El hook IPv4 selecciona una interfaz WiFi o SPI. Las protecciones incorporadas
validan el origen opcional y la disponibilidad de las interfaces; al destruir AP/STA,
se retira su referencia de ruteo en contexto TCP/IP antes de liberar la interfaz.

La definición vigente del paquete está en
[`ring_link_payload.h`](../components/internal/ring_link_lowlevel/include/ring_link_payload.h).
Incluye `id`, tipo, longitud de 16 bits, TTL, origen, destino, buffer y CRC32.
No deducir un formato de serialización portátil del tamaño de sus campos:
la estructura C también puede contener padding.

Los diagramas [payload](arquitectura-payload.svg) y
[buffer](arquitectura-payload.buffer.svg) se conservan como material histórico;
el header del código es la referencia vigente.

La telemetría usa JSON sobre UDP hacia `10.255.255.254:8000` y se recibe mediante
[`gateway/info_server.py`](../gateway/info_server.py). Se conservan la difusión
interna cada 5 minutos y el envío al gateway cada minuto.

Ver [README](../README.md), [contexto del repositorio](../auxiliar/contexto_repo.md)
y [alcance de la integración](comnetar-integration.md).
