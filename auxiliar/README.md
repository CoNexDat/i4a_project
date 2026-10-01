# Flasheo USB desde Raspberry Pi

Estas herramientas permiten instalar por USB el firmware compilado del proyecto,
flashear varios puertos y operar una estación automática con systemd e indicador LED.
El flasheo USB de cada placa es independiente de las actualizaciones del nodo mediante
[gateway/flash_node.py](../docs/actualizacion_firmware.md).

## Compilar y exportar un bundle

Usar ESP-IDF 5.4.1 o el [contenedor](../CONTAINER.md). Desde la raíz:

```bash
idf.py -C app build
python auxiliar/export_firmware_bundle.py --build-dir app/build --output firmware_bundle_idf
```

El exportador copia todos los binarios indicados en `app/build/flash_args`, conserva
sus rutas y offsets y genera un `manifest.json` con hashes SHA-256. Incluye el
bootloader, las particiones, la inicialización de OTA y la aplicación. Rechaza un
destino existente para evitar mezclar compilaciones; elegir otra carpeta para cada versión.

No se distribuyen los binarios históricos de comnetar. El bundle se genera con el
firmware actual. Usar la tabla y capacidad de flash que correspondan a las placas:
el proyecto trae opciones de 4 MB y 2 MB documentadas en la guía de actualización.

## Raspberry Pi: instalación y uso manual

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r auxiliar/requirements-flasher.txt
python auxiliar/esp_rpi_flasher.py list
python auxiliar/esp_rpi_flasher.py flash --port /dev/ttyUSB0 --flash-args firmware_bundle_idf/flash_args
python auxiliar/esp_rpi_flasher.py flash --port /dev/ttyUSB0 --port /dev/ttyUSB1 --flash-args firmware_bundle_idf/flash_args
python auxiliar/esp_rpi_flasher.py monitor --port /dev/ttyUSB0
```

También se puede copiar el bundle a otra máquina y ejecutar el `esp_rpi_flasher.py`
incluido en él con `--flash-args flash_args`. En Windows se admiten puertos explícitos
como `--port COM7`; la autodetección y el servicio están destinados a Linux/Raspberry Pi.

El modo heredado `--bin-dir` con tres binarios y app en `0x10000` corresponde a
firmware sin OTA. No utilizarlo para instalar este proyecto: usar `--flash-args`
para escribir también los datos OTA y respetar los offsets de la compilación.
Una instalación completa por USB puede reinicializar la selección OTA; para
actualizar un nodo ya instalado, seguir la guía de `gateway/flash_node.py`.

## Estación automática

Ver [instalación del servicio](rpi_autoflash_setup.md). El daemon detecta nuevos puertos,
consulta el chip y flashea los que responden como Espressif. La estación debe dedicarse
a las placas que se desea programar: conectar un ESP compatible dispara el flasheo.

El LED queda apagado sin una placa programada conectada, titila durante la escritura
y queda encendido tras el éxito mientras la placa siga conectada. El daemon funciona
sin LED si la Raspberry no ofrece uno compatible en `/sys/class/leds`.
