# EquineRI

EquineRI est une **interface de rendu pour la recherche** au-dessus de Vulkan et de
Direct3D 12, écrite en C++20, avec HLSL comme langage de shaders. Son but est d'accélérer la
recherche : envoyer du travail au GPU doit être trivial, tout en gardant environ 90 % du
contrôle qu'offre une API native ([P1](design-notes/decisions.md#p1)).

!!! info "État d'avancement"
    La phase de conception est terminée et l'implémentation a commencé (jalon 2 du plan).
    Aujourd'hui, RI liste les GPU, crée et détruit des devices, signale les erreurs par un
    canal central unique, et possède le squelette de sa propre validation. Chaque
    fonctionnalité reçoit sa page ici au moment où elle est implémentée : une fonctionnalité
    n'est pas terminée tant que sa page n'existe pas.

## Organisation du site

1. **[Premiers pas](getting-started/build.md)** : compiler la bibliothèque et lancer les tests.
2. **Concepts** : les idées qui traversent plusieurs fonctions, comme
   [le canal d'erreurs](concepts/error-channel.md) et [la validation](concepts/validation.md).
3. **[Référence](reference/index.md)** : une page par fonction ou structure publique.
4. **[Accès natif](native/index.md)** : comment atteindre les objets Vulkan ou D3D12
   derrière RI.
5. **[Aides](helpers/index.md)** : des commodités optionnelles construites sur RI, chacune
   remplaçable par la vôtre.
6. **[Notes de conception](design-notes/index.md)** : le journal des décisions. Les pages
   citent les décisions par leur numéro (P60, P92…).

Les pages qui ne sont pas encore traduites s'affichent en anglais.

## Aussi une ressource pédagogique

La plupart des pages de référence montrent le code RI à côté d'une **version native
canonique** en Vulkan et en D3D12 : la manière de faire la même chose dans votre propre
moteur de rendu. Si vous avez appris avec EquineRI et voulez écrire votre propre petit moteur
Vulkan ou D3D12, ouvrez la page de la fonctionnalité RI que vous connaissez et changez
d'onglet.

=== "RI"

    ```cpp
    --8<-- "snippets/ri/device.cpp:create-device"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/device.cpp:create-device"
    ```

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/device.cpp:create-device"
    ```

Les onglets sont liés : choisissez **Vulkan** une fois et tous les groupes d'onglets du site
passent à Vulkan. Le code natif n'est pas forcément ce que RI fait en interne ; c'est la
manière directe de le faire avec l'API native seule.

## Utiliser le site

- **Thème** : l'icône soleil / lune dans l'en-tête bascule entre clair, sombre et le
  réglage de votre système.
- **Langue** : l'icône en forme de globe change de langue.
- **Version** : une fois les versions publiées, un sélecteur à côté du titre bascule entre
  `latest` (la dernière version publiée) et `dev` (la branche de développement).
- **Hors ligne** : le site construit fonctionne sans serveur. Ouvrez directement
  `index.html` depuis le dossier de construction dans votre navigateur.
