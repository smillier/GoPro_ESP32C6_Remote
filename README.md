# gopro_esp32c6_pairing

Cible ESP-IDF **5.5.4**, chip `esp32c6`.

Firmware de contrôle GoPro par BLE (bonding "Legacy Pairing", protocole
Open GoPro) avec 4 boutons physiques et une LED de statut RGB.

> Une version antérieure incluait un portail WiFi (SoftAP + interface
> web mobile). Elle a été retirée : la cohabitation radio BLE/WiFi sur
> l'ESP32-C6 (une seule antenne partagée) posait trop de problèmes de
> fiabilité (timeouts d'association WiFi pendant que le BLE scanne).
> Le contrôle se fait uniquement via les 4 boutons physiques.

## Architecture

Le firmware est construit autour d'une couche d'état centralisée
(`app_state`) et d'une couche de contrôle partagée (`gopro_control`) :
les boutons physiques appellent des fonctions de contrôle qui
lisent/écrivent un état central — aucune logique dupliquée.

```
                    ┌──────────────┐
                    │   buttons    │
                    │ (GPIO2/3/4)  │
                    └──────┬───────┘
                           │
                           ▼
                   gopro_control.c
                (actions de haut niveau)
                           │
             ┌─────────────┼─────────────┐
             ▼             ▼             ▼
       gopro_ble.c    app_state.c   led_status.c
       (BLE/GATT)    (état partagé)  (LED WS2812)
```

## Structure

```
gopro_esp32c6_pairing/
├── CMakeLists.txt          # projet racine ESP-IDF
├── sdkconfig.defaults       # config NimBLE + bonding, cible esp32c6
├── .vscode/                 # config VSCode + extension ESP-IDF
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml    # pulls in espressif/led_strip (WS2812 driver)
    ├── version.h             # nom/version du firmware
    ├── main.c               # app_main: NVS, host NimBLE, params sécurité
    ├── app_state.c/.h        # état partagé (connexion / enregistrement / mode)
    ├── gopro_control.c/.h    # actions de haut niveau (boutons)
    ├── gopro_ble.c/.h        # scan -> connect -> bonding -> discovery -> commandes
    ├── led_status.c/.h       # LED RGB WS2812 (GPIO8) = statut de connexion
    └── buttons.c/.h          # GPIO2/3/4/18 = shutter / power / mode / reset
```

## Matériel (ESP32-C6 Super Mini)

- **LED RGB WS2812** : déjà intégrée sur la carte, câblée sur **GPIO8**.
  Rien à brancher, le code la pilote directement.
- **4 boutons**, chacun câblé entre son GPIO et **GND** (pull-up
  interne activé dans le code, pas besoin de résistance externe) :
  - **GPIO2** : start/stop enregistrement (shutter)
  - **GPIO3** : marche/veille caméra (power)
  - **GPIO4** : changement de mode (Vidéo → Photo → Timelapse → ...)
  - **GPIO18** : redémarrage de l'ESP32 (reset logiciel, `esp_restart()`)

### Signification des couleurs de la LED

| Couleur                | État                                              |
|-------------------------|---------------------------------------------------|
| Bleu clignotant          | Recherche de la GoPro (mets-la en mode pairing)  |
| Jaune fixe               | Connecté, découverte GATT / bonding en cours      |
| Vert fixe                | Prêt — appui sur le bouton pour démarrer          |
| Rouge clignotant         | Enregistrement en cours                           |

### Comportement des boutons

- **Shutter (GPIO2)** : ignoré tant que la LED n'est pas verte.
  Bascule start/stop à chaque appui.
- **Power (GPIO3)** :
  - Si la GoPro est connectée → envoie la commande `Sleep` (0x05),
    la caméra se met en veille et se déconnecte.
  - Si la GoPro n'est pas connectée → relance immédiatement un scan.
    Une GoPro en veille continue d'annoncer en BLE ; s'y reconnecter
    est ce qui la réveille. **Il n'existe pas de commande BLE
    "power on" séparée** — c'est la reconnexion elle-même qui réveille
    la caméra, donc ce bouton agit bien comme un vrai bouton
    marche/veille dans les deux sens.
- **Mode (GPIO4)** : envoie `Load Preset Group` (0x3E) et fait
  défiler Vidéo → Photo → Timelapse → Vidéo... Ignoré tant que la
  caméra n'est pas prête. La GoPro refuse ce changement pendant un
  enregistrement (comportement normal de la caméra, pas une erreur
  du code).
- **Reset (GPIO18)** : redémarre l'ESP32 (`esp_restart()`), avec un
  court délai pour laisser les logs/réponses en cours se terminer
  proprement.

## Pourquoi ça fonctionne (rappel)

1. La GoPro doit être mise en mode pairing via son écran
   (Préférences → Connexions → Connecter un appareil).
2. On scanne les advertisements filtrés sur le service `0xFEA6`.
3. **Étape clé** : une fois connecté, on découvre d'abord les services/
   caractéristiques GATT, puis on appelle `ble_gap_security_initiate()`
   pour déclencher le bonding (Just Works, pas de saisie de code — la
   GoPro n'a pas d'écran d'entrée). **Confirmé sur cette caméra** :
   il faut utiliser le **Legacy Pairing** (`ble_hs_cfg.sm_sc = 0`)
   plutôt que LE Secure Connections — avec `sm_sc = 1`, la GoPro
   coupait systématiquement la connexion (reason 531 / HCI 0x13) dès
   la demande de sécurité, même après avoir laissé le lien se stabiliser
   via la découverte GATT et réduit la distribution de clés à `ENC`
   seul. Sans bonding réussi, la connexion GATT est acceptée mais la
   GoPro ignore silencieusement toute commande.
4. Une fois le lien chiffré (`BLE_GAP_EVENT_ENC_CHANGE` avec
   `status == 0`), on découvre les caractéristiques du service FEA6
   et on s'abonne (écriture CCCD) à celles en `notify`.
5. `gopro_send_shutter(true/false)` écrit sur la caractéristique
   Command (`0x0072`) pour démarrer/arrêter l'enregistrement.

Les clés de bonding (LTK) sont persistées en NVS
(`CONFIG_BT_NIMBLE_NVS_PERSIST=y`), donc le re-pairing manuel n'est
nécessaire qu'après un factory reset de la caméra.

## Prérequis

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/)
  **5.5.4** (installé via l'extension VSCode ou en CLI). Le code
  n'utilise que des API NimBLE stables communes aux séries 5.x, mais a
  été vérifié spécifiquement contre l'exemple officiel
  `examples/bluetooth/nimble/blecent` de la branche 5.5.
- Extension VSCode "Espressif IDF" (proposée automatiquement à
  l'ouverture du dossier, voir `.vscode/extensions.json`)
- Carte ESP32-C6

## Ouvrir dans VSCode

1. Décompresser le projet, ouvrir le dossier dans VSCode.
2. Installer l'extension ESP-IDF si proposé.
3. `Ctrl+Shift+P` → "ESP-IDF: Configure ESP-IDF extension" (première
   fois seulement) pour pointer vers votre installation IDF/Python.
4. `Ctrl+Shift+P` → "ESP-IDF: Set Espressif Device Target" → `esp32c6`.
5. `Ctrl+Shift+P` → "ESP-IDF: Build your project".
6. Brancher la carte, sélectionner le port série, puis "ESP-IDF: Flash".
7. "ESP-IDF: Monitor" pour voir les logs.

## En CLI (alternative à VSCode)

```bash
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Notes importantes

- **Bluedroid n'est pas supporté sur ESP32-C6** dans ESP-IDF : NimBLE
  est le seul host disponible, c'est déjà configuré dans
  `sdkconfig.defaults`.
- `nimble_port_init()` retourne un `esp_err_t` et gère l'initialisation
  du contrôleur BT en interne (plus besoin d'appeler
  `esp_nimble_hci_init()` séparément) — c'est le comportement depuis
  ESP-IDF 5.0, confirmé sur la branche 5.5.
- Avant de connecter, la GoPro **doit** être en mode pairing (elle
  n'advertise `0xFEA6` que dans cet état, ou pendant les 8h suivant
  une mise en veille).
- Le format de commande shutter (`03 01 01 01` / `03 01 01 00`) vient
  du protocole Open GoPro Command TLV : `[longueur][id_commande]
  [id_param][longueur_param][valeur]`.
- Le premier `idf.py build` télécharge automatiquement le composant
  `espressif/led_strip` (déclaré dans `main/idf_component.yml`) depuis
  le registre de composants ESP-IDF — une connexion internet est donc
  nécessaire au moins pour ce premier build.
- Table de partitions : celle par défaut d'ESP-IDF (1 Mo) suffit
  largement sans WiFi/serveur HTTP.

## Aller plus loin

- Remplacer la structure de découverte GATT simplifiée ici par le
  pattern `peer.c` officiel des exemples `blecent` d'ESP-IDF si vous
  gérez plusieurs services/périphériques en parallèle.
- Si un contrôle à distance reste souhaité malgré les soucis de
  cohabitation radio, envisager un second module (ESP32 classique
  avec Bluedroid, ou un module WiFi externe communiquant par UART/I2C
  avec ce firmware) plutôt que de partager l'unique antenne du C6
  entre BLE et WiFi.
