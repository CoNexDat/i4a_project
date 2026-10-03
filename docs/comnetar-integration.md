# Integración de las mejoras de comnetar

Origen: `adrianfn15/comnetar`, commit `5782ce96891245c3649fe3a5ac9416ebc8bc44a5`.
Base de integración: `CoNexDat/i4a_project`, commit `8df79965ee53d3bd6ff8cf0767b75ac47e51cf24`.

## Alcance

- Hook IPv4 tolerante a origen nulo y destino nulo.
- Comprobación de disponibilidad de interfaces en los hooks de ruteo.
- Retirada de referencias AP/STA en contexto TCP/IP antes de destruir interfaces.
- Pila `sys_evt` configurada en 6144 bytes y pila del ciclo AP/STA en 8192 bytes.
- Conservación de la subred y máscara iniciales durante el ciclo AP/STA.
- Contenedor, wrapper `i4a`, flasheador USB, daemon Raspberry y fuentes de hardware.
- Documentación de arquitectura y diagnóstico importada, con enlaces portables.

Se mantienen el SSID doméstico `ComNetAR`, los intervalos de telemetría del upstream,
el protocolo de ruteo y todo el código y configuración OTA existente. Los binarios
históricos no se incorporan: se exportan bundles desde el build actual, incluyendo
todas las imágenes de `flash_args`. El contenedor usa ESP-IDF 5.4.1.

La protección posterior contra enlaces recíprocos incorpora un transporte de
admisión previo al ruteo y reservas por UUID coordinadas por SPI. Sus requisitos
de actualización y pruebas están en [protección de vecinos](neighbor-link-protection.md).

## Condiciones y validación en placas

La copia de subred/máscara del ciclo AP/STA queda fija durante la vida de esa tarea,
tal como en comnetar. Probar específicamente un reprovisionamiento con otra subred:
la tarea podría restaurar los parámetros iniciales. Este port no rediseña esa política.

`tcpip_callback_wait()` en los destructores debe ejecutarse fuera de la tarea TCP/IP.
Las comprobaciones de interfaz no certifican asociación WiFi ni recepción remota.
Los tamaños de pila necesitan mediciones del margen mínimo bajo carga.

El informe de septiembre (`.md`, `.html`, `.docx`) se conserva como evidencia histórica
del trabajo de Adrián y su compañero en comnetar. Sus versiones de SDK, parámetros de
telemetría y observaciones no constituyen resultados de esta rama. Las pruebas allí
registradas no demuestran estabilidad prolongada de todas las transiciones.

Antes de desplegar: probar conexión, tráfico SPI/WiFi, ciclos AP/STA repetidos,
reprovisionamiento, reinicios coordinados y actualización/rollback OTA, correlacionando
logs de las placas. Mantener una medición de pila y memoria libre bajo carga.

## Comprobaciones reproducibles

```bash
python -m unittest discover -s tests -v
python tests/run_native_ota.py --cc cc
python tests/run_native_neighbors.py --cc cc
idf.py -C app build
```

Las pruebas Python de bundles verifican los offsets, datos OTA, archivos faltantes,
rechazo de sobrescrituras y hashes del manifiesto sin acceder a puertos físicos.
