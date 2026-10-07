# Flujo con contenedor

El repo ahora incluye un wrapper principal:

- [i4a](i4a)

La idea es que no tengas que invocar `docker run ...` a mano. El script construye una imagen local basada en `espressif/idf:v5.4.1`, monta este repo dentro del contenedor y ejecuta todo desde `app/`.

## Requisitos

- Docker instalado
- Acceso al daemon Docker
- En Linux/Raspberry, permisos para usar el puerto serie (`dialout` o equivalente)

## Primer uso

```bash
chmod +x ./i4a
./i4a image build
```

Despues ya podes usar los comandos directos.

## Comandos principales

```bash
./i4a build
./i4a clean
./i4a fullclean
./i4a menuconfig
./i4a shell
./i4a list-ports
./i4a flash /dev/ttyUSB0
./i4a flash-many /dev/ttyUSB0 /dev/ttyUSB1
./i4a erase-flash /dev/ttyUSB0
./i4a monitor /dev/ttyUSB0
./i4a idf build
./i4a idf -p /dev/ttyUSB0 flash
./i4a run python --version
```

## Que hace `./i4a`

- construye la imagen si no existe;
- monta el repo completo en `/workspace`;
- entra a `/workspace/app`;
- ejecuta `idf.py` o el comando que le pidas;
- para flash y monitor monta el puerto serie como dispositivo Docker.

## Notas sobre puertos serie

Para listar puertos:

```bash
./i4a list-ports
```

Si no tenes permisos en Raspberry/Linux:

```bash
sudo usermod -a -G dialout $USER
```

Despues cerra sesion o reinicia.

## Variables utiles

```bash
I4A_IMAGE_NAME=i4a-esp-idf:v5.4.1
I4A_DOCKER_BIN=docker
I4A_FLASH_BAUD=921600
I4A_MONITOR_BAUD=115200
I4A_HOME_CACHE=.i4a-home
I4A_DOCKER_RUN_ARGS="--network host"
I4A_FORCE_REBUILD=1
```

Ejemplo:

```bash
I4A_FORCE_REBUILD=1 ./i4a build
```

## Imagen y entrypoint

Archivos involucrados:

- [Dockerfile](Dockerfile)
- [docker/i4a-container.sh](docker/i4a-container.sh)

El `Dockerfile` usa ESP-IDF `v5.4.1`, que coincide con lo documentado en el proyecto.


## Plataforma y bundles

Los comandos de puertos `/dev/ttyUSB*` requieren Linux/Raspberry Pi o un entorno
con passthrough USB configurado. En Windows con puertos COM, usar ESP-IDF nativo.

La imagen usa 5.4.1 para coincidir con el firmware actual. Los defaults se aplican
al generar la configuración: si ya existe `app/sdkconfig`, comprobar en menuconfig
el tamaño de pila de eventos, la capacidad de flash, las particiones y el rollback.

Después de `./i4a build`, generar un bundle completo desde la raíz:

```bash
python3 auxiliar/export_firmware_bundle.py --build-dir app/build --output firmware_bundle_idf
```

Ver [flasheo USB](auxiliar/README.md) y [OTA del nodo](docs/actualizacion_firmware.md).
