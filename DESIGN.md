# DESIGN.md — Metal Kart Wii Music App

Dirección visual del reproductor. La escribe el dueño del proyecto; el filtro antislop solo aplica encima. Las decisiones marcadas como pendientes no están tomadas: no se deben inventar en el código.

## Identidad

- **Quién usa la app:** aficionados a Mario Kart Wii y sus mods, que escuchan pistas en bucle y a otra velocidad.
- **Qué es:** un reproductor de archivos BRSTM que no genera archivos nuevos.
- **Objetivo:** ofrecer una experiencia de reproducción musical coherente, reconocible y cómoda en todas las plataformas previstas.

## Dials

- **ENERGY 3 (llamativa):** la interfaz debe llamar la atención, con presencia visual y color. La energía debe proceder de la identidad gráfica, el contraste y los elementos destacados, sin convertir todos los controles en elementos que compitan por atención.
- **RHYTHM 1 (uniforme):** bloques del mismo peso y orden predecible. La uniformidad es una elección del dueño. Los bloques principales mantienen una estructura, distribución y peso visual uniformes. Se permite jerarquía dentro de cada bloque para distinguir información y controles, sin alterar el ritmo general.
- **MOTION (pendiente):** solo cambios de estado por ahora. Hasta que se decida lo contrario, no introducir animaciones, transiciones ni efectos de movimiento. Los cambios de estado serán inmediatos.

## Personalidad

- **Retro:** inspiración en reproductores clásicos y consolas.
- **Tono:** nostálgico, sin caer en lo infantil.
- La estética retro debe convivir con una interfaz legible y funcional. No se debe sacrificar usabilidad por imitar interfaces antiguas.

## Paleta

- Sigue el esquema del sistema (claro u oscuro). Los dos deben estar probados.
- Colores concretos: **pendiente**. No se han elegido.
- Regla de contraste: texto normal al menos 4,5:1; texto grande al menos 3:1 (antislop R-25).
- La paleta debe mantener la identidad visual tanto en el tema claro como en el oscuro.
- No introducir colores definitivos, degradados ni efectos decorativos sin una decisión explícita.

## Tipografía

- Todo el texto con una sola fuente incluida en la app, con licencia OFL (SIL Open Font License).
- Fuente elegida: **Pixelify Sans** (SIL OFL 1.1, Copyright 2021 The Pixelify Sans Project Authors).
  Sin modificar. Se distribuye con su texto de licencia.
- La fuente debe incluir su archivo de licencia junto al recurso distribuido.
- Criterios de selección:
  - Buena legibilidad en tamaños pequeños.
  - Cobertura de caracteres españoles: vocales acentuadas, ñ, ¿ y ¡.
  - Diferenciación clara entre caracteres similares, especialmente `0/O` y `1/I/l`.
  - Pesos y estilos suficientes para mantener una jerarquía tipográfica coherente.
- Motivo: Segoe UI solo existe en Windows, y la fuente incluida mantiene el diseño igual en cualquier sistema.
- La estética pixelada no debe prevalecer sobre la legibilidad.

## Principios de interfaz

- La reproducción es el elemento funcional central.
- El estado actual del reproductor debe reconocerse de un vistazo.
- Los controles principales deben ser fáciles de localizar y distinguir.
- La selección de versiones mediante pares estéreo debe resultar comprensible.
- La información secundaria no debe competir visualmente con los controles de reproducción.
- Los estados de reproducción, pausa, carga y error deben distinguirse claramente.
- La interfaz debe conservar una estructura y un comportamiento visual coherentes entre plataformas.
- La uniformidad de los bloques es intencionada y no debe reinterpretarse como una petición de minimalismo.

Estos principios definen prioridades funcionales y visuales, no una distribución concreta de componentes.

## Plataforma y estrategia de lanzamiento

La aplicación está concebida como un reproductor multiplataforma. El desarrollo se realizará por etapas para priorizar la estabilidad y evitar abordar varias implementaciones simultáneamente.

Orden previsto:

1. **Windows:** primera plataforma. Implementación actual mediante Win32 y waveOut. El reproductor está prácticamente terminado.
2. **Android o Web:** segunda plataforma. El orden entre ambas está pendiente de decisión y evaluación técnica.
3. **Linux y macOS:** tercera etapa, una vez consolidada la versión anterior.

### Independencia visual

- La dirección visual debe ser independiente del sistema operativo.
- No depender de controles, fuentes o estilos nativos de una plataforma concreta.
- Incluir una fuente propia con licencia OFL.
- Mantener la identidad visual entre plataformas.
- Adaptar la disposición a las particularidades de cada dispositivo cuando sea necesario, sin perder la identidad gráfica.

La arquitectura multiplataforma y las tecnologías de cada versión se decidirán por separado. Este documento define la dirección visual, no la arquitectura técnica.

## Pendiente antes de aplicar al código

1. Colores concretos para los temas claro y oscuro.
2. Comprobar `1/I/l` con Pixelify Sans en la ventana real, a 16 px, en la biblioteca y en la barra.
3. Movimiento (MOTION): si se quiere alguno y de qué tipo.
4. Orden de desarrollo de la segunda plataforma: Android o Web.

Las decisiones pendientes deben permanecer abiertas. No se deben completar mediante suposiciones durante la implementación.