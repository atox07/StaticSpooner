# Map Spooner 0.1 · GTA V Legacy

Prototipo C++20 per ScriptHookV in modalità Storia. Il progetto predefinito apre la freecam, esegue il raycast, mostra il punto colpito con il gizmo e identifica i bersagli dotati di handle. Per la selezione di una `CBuilding` senza handle e lo spostamento della mappa occorre un adattatore RAGE verificato sulla propria build. Quell'adattatore non è incluso. Il punto di collegamento è `ResolveLegacy`.

## Comandi

| Tasto | Azione |
| --- | --- |
| F12 | Apri o chiudi lo Spooner. Alla chiusura l'eventuale edit temporaneo viene ripristinato |
| Mouse / WASD | Orienta o sposta la freecam |
| Spazio / Ctrl / Shift | Sali, scendi o accelera |
| E | Seleziona il primo collider rilevato lungo il raggio. Ripristina prima l'edit precedente |
| Frecce | Trasla lungo gli assi globali X e Y, con profilo RAGE attivo |
| PagSu / PagGiù | Trasla lungo Z, con profilo RAGE attivo |
| Mouse sinistro sull'asse evidenziato | Trascina l'asse sotto il mirino, con profilo RAGE attivo |
| R | Ripristina la selezione e l'eventuale edit |

Il prototipo tiene un edit temporaneo alla volta. Chiudi lo Spooner con F12 prima di scaricare a caldo l'ASI, così il ripristino gira sulla fibra ScriptHookV. Il raycast lavora sulle collisioni, quindi una mesh priva di collider non è selezionabile mediante questa modalità. In ispezione pura, un hit mondo mostra il punto del collider e un prop con handle mostra hash e indirizzo restituito dallo SDK.

## Compilazione

### GitHub Actions

Metti il contenuto della cartella `MapSpooner` nella radice del repository, includendo `.github/workflows/build.yml`. Quando il workflow è sul branch predefinito, apri **Actions → Build MapSpooner → Run workflow** e scegli il branch. La compilazione parte soltanto su richiesta.

Il workflow usa Windows x64 e Visual Studio 2022, compila in Release ed esegue i test del core. Al termine, scarica l'artifact `MapSpooner-Legacy-x64` contenente `MapSpooner.asi`. L'SDK viene scaricato dai link del distributore; in alternativa, il workflow usa una copia dell'SDK ufficiale estratto in `sdk/ScriptHookV` nel repository.

### Compilazione locale

Richiede CMake, Visual Studio 2022 con C++ desktop e SDK ufficiale ScriptHookV (`inc/main.h`, `lib/ScriptHookV.lib`).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DSCRIPTHOOKV_SDK="C:/SDK/ScriptHookV"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Il risultato previsto è `build/Release/MapSpooner.asi`, da mettere accanto a `GTA5.exe` con ScriptHookV e il loader ASI. Lo scanner usa l'immagine PE di `GTA5.exe`. Il sorgente del profilo verificato si passa alla configurazione con `-DMAP_SPOONER_LEGACY_ADAPTER="C:/src/LegacyBuild.cpp"` e sostituisce `src/Legacy.cpp`.

Per compilare solo i test C++ della logica LOD, senza SDK, configura con `-DMAP_SPOONER_BUILD_ASI=OFF`.

## Reverse engineering del profilo Legacy

`Rage.hpp` contiene snapshot tipizzati delle viste `CEntity`, `fwArchetype` e `phInst`. I tipi originali sono opachi. Le viste parziali in `reference` riportano come confronto i campi derivabili dal sorgente FiveM: puntatore all'archetipo a `+0x20`, matrice inline a `+0x60` e hash dell'archetipo a `+0x18`. Conferma queste posizioni e ogni ABI prima dell'uso su Legacy. FiveM mostra inoltre `SetTransform` e `UpdateTransform` come chiamate virtuali con mapping per build. Non usare i relativi offset di vtable come costanti Legacy. Un cache di bounds per istanza si aggiorna separatamente dai bounds locali dell'archetipo, che può essere condiviso da più edifici.

`Pattern.hpp` espone `Image::Unique` per cercare una firma IDA contenente byte e wildcard nelle sezioni eseguibili PE. Una firma con zero o più di un match restituisce zero. `Image::Relative` risolve un operando signed rel32 o RIP-relative da un'istruzione già identificata. Nel profilo, controlla file version e fingerprint PE, funzione e callsite attesi, ABI Microsoft x64 e tipo di ogni target. Le firme byte esatte per l'eseguibile corrente sono da ricavare dalla disassembly. La tabella indica i punti da ancorare e i controlli da fare.

Il pattern scanning può riusare lo stesso profilo tra aggiornamenti quando le istruzioni riconoscibili e la semantica rimangono compatibili. Per evitare offset fissi, escludi dalla firma i displacement che possono cambiare e ricavali dagli operandi del match. Per una routine spostata, risolvi la destinazione dal `call rel32`. Il profilo deve comunque confermare la struttura delle istruzioni, il tipo di oggetto e il comportamento della routine. Se in una nuova build cambia il layout o l'ABI, servirà una firma o una verifica aggiornata prima di riabilitare le scritture.

| Firma da ricavare | Ancoraggio da cercare | Verifica sul runtime |
| --- | --- | --- |
| Risoluzione hit fisico → istanza | Dentro la routine chiamata da `START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE`, cerca dove il risultato fisico diventa uno script GUID. Conserva l'identità del collider prima della conversione | Confronta il medesimo raggio su un prop e su un edificio. Controlla classe, owner, eventuale child bound e generazione. Un collider world/YBN aggregato può non avere una singola CBuilding corrispondente |
| Link LOD runtime | Routine che collega le entità quando esegue lo streaming della YMAP. Leggi il displacement effettivo dell'accesso al parent, il tipo LOD e il conteggio/uso dei figli | Confronta con `CEntityDef.parentIndex` e `lodLevel` in CodeWalker, comprese relazioni fra YMAP. Verifica se quello chiamato `m_pLod` sia davvero un puntatore sulla build |
| Archetipo e trasformazione | Accessor dell'archetipo e chiamante di `SetTransform` / `UpdateTransform`. Cerca l'accesso a quattro righe e la traslazione nell'ultima | Controlla matrice inline vs indiretta, offset/rappresentazione e posizione contro un'istanza nota. Verifica che l'archetipo appartenga al modello atteso |
| Aggiornamento spaziale | Chiamate eseguite dopo la modifica della matrice che rigenerano bounds e notificano al sistema scena o streaming l'aggiornamento dell'istanza | Prova visibilità oltre la cella d'origine e al cambio di distanza. Distingui sphere/AABB dell'istanza dalla registrazione spaziale e dall'estensione della YMAP. Se il proprietario resta vincolato al settore originale, limita gli spostamenti al settore verificato |
| Collisione statica | Comandi con cui il motore rimuove e reinserisce l'istanza fisica quando scarica/carica il settore | Individua il momento sincronizzato con `phSimulator`, il proprietario della shape e il ripristino. Escludi le collisioni aggregate. Scrivere sotto terra le sole coordinate di `phInst` può lasciare il broadphase incoerente |
| Durata delle istanze | Registrazione/invalidazione del pool o gestore delle istanze di streaming, più il punto di aggiornamento comune scena/fisica | Verifica riuso dello stesso indirizzo dopo un unload. L'adattatore deve trattenere tutte le istanze o rifiutare l'edit se non può garantire ripristino e validità |

### Contratto dell'adattatore

- `pick` riceve il raggio, l'hit e l'eventuale handle. Con un handle usabile, `getScriptHandleBaseAddress` fornisce un candidato. L'adattatore deve confermare che è una `CBuilding`. Per l'hit senza handle deve recuperare la vera identità del collider con la query fisica interna o un hook sul suo risultato, senza dedurla dal modello più vicino.
- `inspect` restituisce matrice, archetipo, parent, LOD, bounds world-space e metadati di collisione. Un parent assente va rappresentato con ID nullo. Un parent esistente ma non risolvibile rende la catena non editabile.
- `acquire` ricontrolla identità, generazioni e foto iniziali della catena. `apply` applica insieme i target e aggiorna le registrazioni del motore. La prima applicazione toglie la collisione originale tramite la routine verificata; `restoreAndRelease` ripristina tutte le matrici, registrazioni e collisioni originali, poi rilascia il lease.
- Il core costruisce ogni target dalle matrici e dai bounds originali più un delta assoluto, così i livelli mantengono posizione relativa e base di rotazione. Si ferma a SLOD1, esclude SLOD2 e rifiuta i parent condivisi o fusi. Per geometria di distanza inclusa in SLOD2, a distanza molto grande continuerà a valere il settore originale.
- Il controller attende modello e collisioni, crea con `CREATE_OBJECT_NO_OFFSET` un oggetto locale non networkato, lo congela, nasconde e allinea alla rotazione originale. Lo usa come proxy soltanto quando il profilo dichiara equivalente la collisione del modello e conferma scala unitaria. Se il collider è baked in YBN o il modello non è istanziabile come `CObject`, il proxy non è un sostituto affidabile. La versione iniziale rifiuta questi casi.

## Riferimenti

- [ScriptHookV e SDK ufficiale](https://www.dev-c.com/gtav/scripthookv/)
- [FiveM · EntitySystem.h](https://github.com/citizenfx/fivem/blob/master/code/components/gta-streaming-five/include/EntitySystem.h)
- [CodeWalker · YmapFile.cs](https://github.com/dexyfex/CodeWalker/blob/master/CodeWalker.Core/GameFiles/FileTypes/YmapFile.cs)
- [Cfx Native Reference · GET_SHAPE_TEST_RESULT](https://github.com/citizenfx/natives/blob/master/SHAPETEST/GetShapeTestResult.md)
- [Cfx Native Reference · CREATE_OBJECT_NO_OFFSET](https://github.com/citizenfx/natives/blob/master/OBJECT/CreateObjectNoOffset.md)
