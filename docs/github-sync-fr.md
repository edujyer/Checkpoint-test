# Checkpoint GH pour 3DS

Version modifiée de Checkpoint, avec synchronisation GitHub.
Auteur original : Bernardo Giordano et les contributeurs de Checkpoint/FlagBrew.
Licence : GPLv3, avec les mentions et conditions additionnelles du projet original.

## Installation séparée

- Nom HOME : **Checkpoint GH**
- Unique ID : **0xED071**
- TitleID : **000400000ED07100**
- Code produit : **CTR-HB-CKGH**
- Checkpoint original : **000400000BCFFF00**.

Le CIA s'installe à côté de Checkpoint. Il utilise les mêmes backups dans
/3ds/Checkpoint/saves et /3ds/Checkpoint/extdata.
Les réglages GitHub sont séparés dans /3ds/Checkpoint/github.json.
Les préférences de cette édition sont dans /3ds/Checkpoint/github-edition-config.json ;
le fichier config.json de l'original est conservé.
Les mises à jour automatiques de l'application originale sont désactivées dans
cette édition, pour éviter de télécharger un CIA avec l'identité de l'original.

## Configuration (une fois)

1. Crée un dépôt GitHub **privé**, distinct du dépôt public contenant ce code.
   Coche « Add a README » pour initialiser sa branche main.
2. Crée un token GitHub **fine-grained**, limité à ce seul dépôt.
   Permission nécessaire : **Contents / Read and write**.
   Aucune permission Actions, Workflows ou Administration n'est nécessaire.
3. Sur la SD, crée /3ds/Checkpoint/github-token.txt.
   Colle uniquement le token dans ce fichier. Un retour à la ligne final est accepté.
4. Ouvre Checkpoint GH : SELECT → Paramètres → GitHub.
   Indique ton-compte/ton-depot-prive et la branche (main par défaut).
5. Active **Envoyer après chaque backup** si tu veux l'envoi automatique.

Le token est lu depuis la SD. Il n'est ni intégré au CIA, ni envoyé au dépôt,
ni affiché dans les menus, ni écrit dans les journaux. Il reste lisible par
quelqu'un ayant accès à ta SD : ne partage pas ce fichier.
Ne copie jamais le token dans les sources ou dans un dépôt GitHub.

## Utilisation

- **Automatique** : après chaque backup local réussi, l'application envoie un ZIP.
  L'option est désactivée par défaut. Une erreur réseau conserve le backup local
  et affiche un avertissement. Réessaie depuis le menu GitHub.
- **Envoi manuel** : sélectionne un jeu puis un backup, SELECT → GitHub →
  Envoyer le backup sélectionné.
- **Récupération** : sélectionne un jeu et l'onglet Sauvegarde ou Extdata,
  SELECT → GitHub → Récupérer un backup GitHub.
  Choisis le fichier, puis confirme. Une nouvelle copie est créée sur la SD.
- **Restaurer dans le jeu** : sélectionne ensuite cette copie et utilise
  Restaurer dans Checkpoint. Le téléchargement seul ne modifie pas la sauvegarde du jeu.

Structure distante :
checkpoint/3ds/<TitleID du jeu>/<save ou extdata>/<nom du backup>.zip.
Réenvoyer un backup portant le même nom crée un nouveau commit :
les anciennes versions restent dans l'historique GitHub.
Une modification concurrente provoque une erreur plutôt qu'un écrasement forcé.

## Limites et vérification

- Wi-Fi et date de la console corrects requis. HTTPS vérifie le certificat et le nom d'hôte.
- Le dépôt de destination doit rester privé ; une destination publique est refusée.
- ZIP limité à **32 Mio**, **4096 entrées**. Une liste de **1000 fichiers** ou plus
  est refusée pour éviter de masquer des sauvegardes.
- Le nom du backup, avec .zip, doit faire au plus **180 octets**.
- Prévois jusqu'à **75 Mio libres** sur la SD pour préparer l'envoi d'un ZIP de 32 Mio.
- L'envoi suit les backups lancés depuis l'interface Checkpoint (y compris les lots).
  Un script qui écrit directement des fichiers ne déclenche pas cet envoi.
- Pas de suppression distante ni de restauration automatique.
- Une sauvegarde partielle d'extdata signalée par Checkpoint reste partielle sur GitHub.
- Ce fork ne permet pas de reprendre un transfert interrompu.
- Les tests automatiques vérifient le code et le TitleID du CIA. Le Wi-Fi, le token,
  la coexistence sur HOME et la restauration doivent aussi être testés sur une vraie 3DS.

## Compilation

Le workflow **Build Checkpoint GH** produit l'archive **Checkpoint-GH-3DS**
dans l'onglet Actions du dépôt. Elle contient le CIA et le 3DSX.
Aucun token personnel n'est nécessaire pour compiler.

Pour compiler localement : installe devkitARM, les bibliothèques indiquées
dans le README original, makerom et bannertool. Télécharge le bundle de CA
Mozilla depuis https://curl.se/ca/cacert.pem dans 3ds/assets/romfs/github-ca.pem,
puis lance make 3ds.
