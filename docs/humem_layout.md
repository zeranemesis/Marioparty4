# Format exact d'une allocation HuMem (x86-64)

Établi par lecture de `src/game/memory.c` et `src/game/malloc.c` **avant** toute
modification. Aucun offset de ce document n'est supposé : chacun se déduit de la
structure réelle et des macros du fichier.

## En-tête de bloc

`src/game/memory.c:20` :

```c
struct memory_block {
    s32 size;                    /* 0..3   */
    u8  magic;                   /* 4      */
    u8  flag;                    /* 5      */
    /* 2 octets de remplissage      6..7   */
    struct memory_block *prev;   /* 8..15  */
    struct memory_block *next;   /* 16..23 */
    uintptr_t num;               /* 24..31 */
    uintptr_t retaddr;           /* 32..39 */
};                               /* sizeof == 40 */
```

## Charge utile et alignement

Branche 64 bits des macros (`INTPTR_MAX != INT32_MAX`) :

| Macro | Valeur |
|---|---|
| `MEM_ALLOC_SIZE(size)` | `((size - 1) / 32 + 1) * 32 + 64` |
| `BLOCK_GET_DATA(block)` | `(char *)block + 64` |
| `DATA_GET_BLOCK(ptr)` | `(char *)ptr - 64` |
| `BLOCK_ALIGNMENT` | 64 |

Donc :

- la **charge utile commence à l'offset 64** du bloc ;
- `size` est la **taille TOTALE du bloc, en-tête de 64 octets compris** ;
- la capacité utile vaut `size - 64`, arrondie au multiple de 32 supérieur ;
- **les offsets 40 à 63 ne sont utilisés par rien** : `sizeof(struct
  memory_block)` vaut 40 alors que la charge utile démarre à 64. Ce sont
  **24 octets morts dans chaque bloc**, disponibles pour un diagnostic sans
  déplacer une seule adresse.

Exemple : demander 90 octets donne `((89)/32 + 1) * 32 + 64 = 160`. Le bloc fait
160 octets, la charge utile 96, dont 6 de mou après les 90 demandés.

## Signification de `magic` et `flag`

| Valeur | Sens |
|---|---|
| `magic == 205` (0xCD) | bloc **libre** |
| `magic == 165` (0xA5) | bloc **alloué** |
| `flag == 0` | libre |
| `flag == 1` | alloué |

Les deux sont redondants ; une incohérence entre eux est déjà une corruption.
`HuMemHeapInit` initialise le bloc unique à `magic = 205`, `flag = 0`,
`prev = next = block`, `retaddr = 0xCDCDCDCD`.

## Liste chaînée et ordre physique

La liste est **circulaire** et suit **l'ordre physique des adresses**. La
découpe (`memory.c:65`) place le nouveau bloc à `(char *)block + alloc_size`,
c'est-à-dire immédiatement après le bloc courant.

Conséquence capitale pour le diagnostic : **la fin de la charge utile d'un bloc
est exactement l'en-tête du bloc suivant.** Un débordement de charge utile
écrase donc `next->size`, `next->magic`, `next->flag`, puis ses pointeurs. Une
simple validation d'en-tête détecte le débordement du voisin précédent sans
qu'aucun canari de queue soit nécessaire.

La fusion au `free` (`memory.c:123-135`) le confirme : elle teste
`block->prev < block` et `block->next > block`, donc suppose que le chaînage
respecte l'ordre des adresses.

## Allocation

Premier ajustement (`first fit`) en parcourant la liste circulaire depuis
`heap_ptr`. Un bloc est retenu si `!flag && size >= alloc_size`. Il est découpé
si `size - alloc_size > 64`. Puis `flag = 1`, `magic = 165`, `num` et `retaddr`
sont renseignés.

## Libération

Exige `magic == 165`, sinon refus avec message. Fusionne avec le précédent s'il
est physiquement avant et libre, puis avec le suivant s'il est physiquement
après et libre. Termine sur `flag = 0`, `magic = 205`, `retaddr` mis à jour.

**Il n'existe pas de `realloc`.**

## Tas

`src/game/malloc.c` : cinq tas, tailles `{ 0x240000, 0x140000, 0xA80000,
0x580000, 0 }` **multipliées par 4 sous `TARGET_PC`**. Le cinquième prend le
reste. `HeapTbl[i]` donne la base, `HeapSizeTbl[i]` la taille.

| Tas | Constante | Taille PC |
|---|---|---|
| `HEAP_SYSTEM` | 0x240000 | 9 437 184 |
| `HEAP_MUSIC` | 0x140000 | 5 242 880 |
| `HEAP_DATA` | 0xA80000 | 44 040 192 |
| `HEAP_DVD` | 0x580000 | 23 068 672 |
| `HEAP_MISC` | reste | variable |

Tous sont obtenus par `OSAlloc`, donc découpés **à l'intérieur du bloc MEM1
unique de 64 Mo**. C'est précisément pourquoi AddressSanitizer et Full PageHeap
sont aveugles à un débordement d'un bloc HuMem vers son voisin : pour eux,
l'accès reste dans une allocation valide.

## Conséquence pour l'instrumentation

Le format permet une détection **sans aucun changement de disposition** :

1. les 24 octets morts aux offsets 40..63 reçoivent une zone rouge de tête, qui
   détecte une écriture sous la charge utile ;
2. la contiguïté physique fait qu'un débordement de queue corrompt l'en-tête
   suivant, que la validation d'en-tête détecte ;
3. le mou entre la taille demandée et la capacité arrondie, quand il existe,
   reçoit une zone rouge de queue ;
4. tout le reste (identifiant, taille demandée, frame, overlay) vit dans une
   table parallèle **hors de MEM1**.

Aucune adresse de bloc ne bouge, ce qui préserve la reproductibilité du crash.
