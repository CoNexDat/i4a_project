# Estación automática de flasheo

Primero generar el bundle según [README](README.md). En la Raspberry, preparar:

```text
/home/i4a/i4a-flasher/
  .venv/
  esp_rpi_flasher.py
  rpi_esp_autoflash_daemon.py
  install_rpi_autoflash.sh
  i4a-rpi-autoflash.service
  requirements-flasher.txt
  firmware_bundle_idf/
    flash_args
    manifest.json
    ... todas las imágenes exportadas, con sus subdirectorios
```

Copiar los scripts y `requirements-flasher.txt` desde `auxiliar/`, junto con el bundle
completo. El usuario `i4a` y la ruta son ejemplos; el instalador permite cambiarlos.

```bash
cd /home/i4a/i4a-flasher
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-flasher.txt
sudo .venv/bin/python rpi_esp_autoflash_daemon.py --flash-args firmware_bundle_idf/flash_args
```

Después de comprobar el funcionamiento en primer plano, finalizar con Ctrl+C e instalar:

```bash
sudo bash install_rpi_autoflash.sh --flash-args /home/i4a/i4a-flasher/firmware_bundle_idf/flash_args
sudo systemctl status i4a-rpi-autoflash.service
sudo journalctl -u i4a-rpi-autoflash.service -f
```

Para otra ubicación, proporcionar `--workdir`, `--python` y `--daemon` con rutas
absolutas. El instalador actual espera rutas sin espacios. El servicio se ejecuta
como root para acceder a los puertos y al LED de actividad.

```bash
sudo systemctl stop i4a-rpi-autoflash.service
sudo systemctl disable i4a-rpi-autoflash.service
```

El daemon intenta cada puerto una vez por conexión. Para reintentar tras un fallo,
desconectar y reconectar la placa. Un flasheo exitoso confirma la escritura, no el
arranque ni la conectividad del firmware: verificar luego por monitor y telemetría.
