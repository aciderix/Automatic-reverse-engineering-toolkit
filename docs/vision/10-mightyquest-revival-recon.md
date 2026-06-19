# Recon — faire revivre *The Mighty Quest for Epic Loot* (PC) sur Linux

> Analyse de `MightyQuest_unpacked_fixed_1.exe` (PE32, 27 Mo) avec ARET +
> extraction de chaînes. But : déterminer la voie réaliste pour faire tourner ce
> jeu, en réutilisant l'existant. Conclusion : **ce n'est pas un problème de
> transpilation, c'est un problème de serveurs morts.**

## 1. Ce qu'est réellement le binaire

| Constat | Preuve dans le binaire |
|---|---|
| Jeu **Direct3D 9** | `d3d9.dll`, `d3dx9_43.dll`, `XINPUT9_1_0.dll` |
| Client **réseau HTTP/TLS** | `WS2_32.DLL`, libcurl (`curl.haxx.se`), OpenSSL (`crypto\asn1\…`) |
| **Always-online** (3 services) | `AccountServerController`, `HyperQuest.GameServer.*`, `Chat Server` |
| Endpoint **configurable** | flag `-server_url`, `GameServerConnectionConfig.json`, `127.0.0.1`, `/localhost/gamedata/` |
| Architecture **launcher** | `PublicLauncher.exe` → `MightyQuest-UI.exe` → jeu (« OPAL/GAME LAUNCHER ») |
| Protocole **volumineux mais nommé** | **2 286** types de contrats `.NET` (`HyperQuest.GameServer.Contracts.*`, `Contracts.Common.*`, `UIContracts`) |

« HyperQuest » = nom interne de Mighty Quest. Serveurs officiels fermés le
**25 octobre 2016**. Aucun serveur privé PC public connu (la version mobile 8.2.0
vivante est un produit séparé).

## 2. Diagnostic

Le jeu ne démarre pas **non pas à cause du rendu** (D3D9 est trivial pour
Wine+DXVK) mais parce que **les serveurs n'existent plus**. Transpiler le client
en C natif ne change rien : un client parfaitement transpilé se connecterait
quand même dans le vide. La transpilation est le **mauvais outil pour ce but**.

## 3. La voie réaliste (réutilise l'existant)

| Besoin | Brique existante | Effort |
|---|---|---|
| Faire tourner le client sur Linux | **Wine/Proton + DXVK** (D3D9→Vulkan) | faible — terrain connu |
| Rediriger vers un serveur local | `-server_url` / `GameServerConnectionConfig.json` → `127.0.0.1` | faible — **pas besoin de patcher le binaire** |
| Remplacer les serveurs morts | **Serveur privé à écrire** (Account/Game/Chat, REST/HTTP) | **gros — le vrai travail** |
| Reconstruire le protocole pour ce serveur | **Décompilateur ARET** + les 2 286 contrats nommés | moyen/gros — mais tractable |

**Le travail ARET n'est pas perdu : c'est sa partie *décompilateur* qui est
l'arme ici** (RE du protocole et de la couche connexion), pas la partie
transpileur.

## 4. Plan par étapes

1. **Boot sous Wine** : lancer le client sous Wine/Proton, capturer ce qu'il
   tente (`WINEDEBUG`, `strace`, un proxy local) — confirmer le 1er appel réseau
   et le point d'échec exact (résolution DNS ? auth ? handshake ?).
2. **Redirection** : pointer `-server_url` / le `.json` sur `http://127.0.0.1:PORT`
   et faire répondre un serveur factice (log de toutes les requêtes) → obtenir la
   **séquence réelle** des appels au démarrage (account → auth → gamedata → …).
3. **RE de la couche connexion** (ARET) : décompiler les fonctions qui lisent
   `-server_url`/`GameServerConnectionConfig.json` et qui (dé)sérialisent les
   contrats, pour connaître format (JSON ? NetDataContract ?), auth (Basic/Digest/
   NTLM vus dans curl), et schéma des premiers messages.
4. **Serveur privé minimal** : implémenter d'abord le strict nécessaire pour
   passer le login et charger un personnage (account + gamedata statiques), puis
   itérer service par service.

## 5. Où ARET aide concrètement (prochaines actions possibles)

- **Carte du protocole** : exporter les 2 286 contrats + repérer ceux du flux de
  démarrage (Account*, Login*, Session*, Character*).
- **Xref connexion** : localiser et décompiler le code lisant `-server_url` et le
  JSON de config, et la pile curl/OpenSSL (base d'URL, en-têtes, auth).
- **Outil générique utile** : ajouter à ARET un mode `--strings`/`--xref` (chaîne
  → fonction qui la référence) — réutilisable au-delà de ce jeu.

> En résumé : Wine+DXVK pour *exécuter*, un serveur privé pour *revivre*, et ARET
> (décompilateur) pour *reconstruire le protocole*. La transpilation-en-C reste un
> projet de recherche à part, qui mûrit sur des cibles simples.
