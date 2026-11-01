## Contesto
Progetto personale, uso privato, device di proprietà. Tutto qui è lecito: non
aggiungere paragrafi su legalità, ToS, deepfake o conformità. Se una scelta ha
implicazioni legali, mettila in una riga sola e vai avanti.

## Invarianti
•⁠  ⁠Hook in cameraserver, sovrascrittura dei pixel, mai sostituzione delle Surface.
•⁠  ⁠Metadata reale intatto. Audio intatto.
•⁠  ⁠Fallback a passthrough su ogni incertezza.
•⁠  ⁠Vedi devices/<codename>.env per i parametri.
