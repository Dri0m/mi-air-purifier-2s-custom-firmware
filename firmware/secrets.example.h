/* Copy to secrets.h (gitignored) and fill in. */
#define WIFI_SSID      "your-ssid"
#define WIFI_PASSWORD  "your-password"
/* Optional rescue network, e.g. a phone hotspot you can always bring up, so
 * a router SSID/password change doesn't need a disassembly.  Tried after
 * 2 min without an IP on the primary (then alternating every 2 min while
 * neither works); while on it, the primary is retried every 30 min (about a
 * minute offline if that fails).  Leave commented out (or "") for none.
 * Password "" = open network. */
/* #define WIFI_SSID_2     "rescue-ssid" */
/* #define WIFI_PASSWORD_2 "rescue-password" */
#define API_PASSWORD   ""              /* ESPHome API password, "" = none */
/* ESPHome API encryption key (Noise), same as the YAML "api: encryption:
 * key:".  Base64 of 32 random bytes: `openssl rand -base64 32`.  Enter the
 * same key in Home Assistant when adding the device.  Set: only encrypted
 * clients are accepted (plaintext ones are rejected; an invalid key disables
 * the API instead of falling back to plaintext).  "" = plaintext API. */
#define API_ENCRYPTION_KEY ""
#define OTA_PASSWORD   "change-me"     /* required for POST /update */
#define DEVICE_NAME    "air-purifier-2s"
