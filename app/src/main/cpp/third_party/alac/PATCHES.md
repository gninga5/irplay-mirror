# Modifiche rispetto all'originale Apple

Sorgenti presi da https://github.com/macosforge/alac (cartella `codec/`), commit in
`UPSTREAM_COMMIT`, licenza Apache-2.0 (`LICENSE`). Sono inclusi solo i file del decoder.

## ALACDecoder.cpp: limite su `numSamples` (sicurezza)

Con il flag `partialFrame` il pacchetto stesso indica quanti campioni contiene (32 bit).
L'originale non confronta questo valore con `frameLength`, la dimensione dei buffer interni
(`mMixBufferU`, `mMixBufferV`, `mPredictor`, ...): un pacchetto costruito ad arte fa scrivere oltre
la fine di quei buffer. Dopo la lettura di `numSamples` (elementi SCE/LFE e CPE) è stato
aggiunto:

    RequireAction( numSamples <= mConfig.frameLength, status = kALAC_ParamError; goto Exit; );

Trovato con il fuzzing in `tools/hosttest` (AddressSanitizer).

## Limiti di lettura (nessuna modifica al codice)

Il lettore di bit (`ag_dec.c`) non controlla la fine del buffer. Con `numSamples` limitato,
la lettura massima oltre l'inizio del pacchetto resta sotto gli 8 KB: `AlacDecoder` in
`media_streams.cpp` decodifica sempre da una copia con 16 KB di zeri in coda.
