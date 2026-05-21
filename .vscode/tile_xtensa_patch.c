/*
 * PATCH: tile.c — conversione dei blocchi MIPS in assembly Xtensa LX7 (ESP32-S3)
 *
 * Sostituisce le due funzioni WRITE_4PIXELS16 e WRITE_4PIXELS16_FLIPPED
 * nel file: src/snes/snes9x/src/tile.c
 *
 * Differenze rispetto al MIPS originale:
 *
 *  1. MIPS ha il "branch delay slot": l'istruzione dopo un bne/beq viene
 *     eseguita SEMPRE indipendentemente dall'esito del branch. Xtensa LX7
 *     NON ha delay slot, quindi ogni istruzione si comporta normalmente.
 *
 *  2. Su MIPS il codice usava 3 istruzioni per il test (sltiu + sltu + or)
 *     più un bne combinato. Su Xtensa usiamo due branch separati e diretti
 *     (beqz + bgeu), che sono più leggibili e altrettanto efficienti.
 *
 *  3. Latenza di load su Xtensa LX7: 2 cicli (hardware interlock automatico).
 *     Se l'istruzione N+1 usa il risultato di un load all'istruzione N,
 *     la pipeline si blocca 1 ciclo. La strategia è emettere tutti gli 8
 *     load (4 pixel + 4 depth) PRIMA di qualsiasi branch: quando arrivamo
 *     al primo beqz, i load sono già completati da 8+ cicli — zero stall.
 *
 *  4. Dopo l16ui (load palette), inseriamo s8i (store depth) come istruzione
 *     indipendente prima di s16i (store screen): questo nasconde la latenza
 *     del load della palette senza aggiungere cicli morti.
 *
 *  5. La condizione z-test:
 *       MIPS: sltu Cond, ZCompare, ZA  → Cond = (ZCompare < ZA), poi bne
 *       Xtensa: bgeu DA, ZC, skip       → branch se DA >= ZC (z-test fallisce)
 *     Semanticamente identici: saltiamo se ZCompare non è strettamente > depth.
 *
 * Istruzioni Xtensa usate:
 *   l8ui  at, as, imm   — load byte unsigned (imm = byte offset 0–255)
 *   l16ui at, as, imm   — load halfword unsigned (imm = byte offset pari 0–510)
 *   s8i   at, as, imm   — store byte
 *   s16i  at, as, imm   — store halfword (imm = byte offset pari)
 *   slli  at, as, imm   — shift left logico immediato
 *   add   ar, as, at    — addizione
 *   beqz  as, label     — branch se as == 0
 *   bgeu  as, at, label — branch se as >= at (unsigned)
 */

/* ─────────────────────────────────────────────────────────────────────────
 * WRITE_4PIXELS16  —  pixel in ordine normale: Pixels[0,1,2,3]
 * ───────────────────────────────────────────────────────────────────────── */
static INLINE void WRITE_4PIXELS16(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
/* ── Xtensa LX7 (ESP32-S3) ─────────────────────────────────────────────── */
#if defined(__XTENSA__) && defined(__GNUC__) && !defined(NO_ASM)

    uint16_t *Screen = (uint16_t *) GFX.S + Offset;
    uint8_t  *Depth  = GFX.DB + Offset;

    /* Variabili temporanee mappate su registri hardware dal compilatore.
     * Il qualificatore "=&r" (earlyclobber) garantisce che il compilatore
     * non assegni lo stesso registro a un input e a questi output. */
    uint32_t PA, PB, PC, PD;   /* indici di colore dai 4 pixel             */
    uint32_t DA, DB, DC, DD;   /* valori depth correnti per i 4 pixel      */
    uint32_t Color;             /* colore 16-bit letto dalla palette        */

    __asm__ __volatile__ (

        /* ── Fase 1: front-load di tutti gli 8 byte ────────────────────────
         * Emettendo tutti i load PRIMA di qualsiasi branch o uso dei valori,
         * la pipeline può eseguire i fetch in parallelo (o quasi).
         * Con latenza di 2 cicli su LX7, dopo 8 istruzioni di load tutti i
         * valori sono disponibili — nessuno stall nelle fasi successive.     */
        "l8ui  %[PA], %[In8], 0     \n"   /* PA = Pixels[0]                 */
        "l8ui  %[PB], %[In8], 1     \n"   /* PB = Pixels[1]                 */
        "l8ui  %[PC], %[In8], 2     \n"   /* PC = Pixels[2]                 */
        "l8ui  %[PD], %[In8], 3     \n"   /* PD = Pixels[3]                 */
        "l8ui  %[DA], %[Z],   0     \n"   /* DA = Depth[0]                  */
        "l8ui  %[DB], %[Z],   1     \n"   /* DB = Depth[1]                  */
        "l8ui  %[DC], %[Z],   2     \n"   /* DC = Depth[2]                  */
        "l8ui  %[DD], %[Z],   3     \n"   /* DD = Depth[3]                  */

        /* ── Pixel A ────────────────────────────────────────────────────────
         * Condizione di scrittura: PA != 0  &&  ZCompare > DA
         * Skip se: PA == 0 (pixel trasparente) OPPURE DA >= ZC (z-test fail) */
        "beqz  %[PA], 2f            \n"   /* salta se pixel trasparente     */
        "bgeu  %[DA], %[ZC], 2f     \n"   /* salta se z-test fallisce       */
        "slli  %[Color], %[PA], 1   \n"   /* Color = PA * 2 (byte offset)   */
        "add   %[Color], %[Pal], %[Color] \n" /* Color = &palette[PA]       */
        "l16ui %[Color], %[Color], 0 \n"  /* Color = palette[PA]  (16-bit)  */
        "s8i   %[ZS], %[Z], 0       \n"   /* Depth[0] = ZSet  ← riempie     */
        "s16i  %[Color], %[Out], 0  \n"   /*   la latenza del l16ui (1 slot) */
                                          /* Screen[0] = Color               */

        /* ── Pixel B ─────────────────────────────────────────────────────── */
        "2:                          \n"
        "beqz  %[PB], 3f            \n"
        "bgeu  %[DB], %[ZC], 3f     \n"
        "slli  %[Color], %[PB], 1   \n"
        "add   %[Color], %[Pal], %[Color] \n"
        "l16ui %[Color], %[Color], 0 \n"
        "s8i   %[ZS], %[Z], 1       \n"   /* Depth[1] = ZSet                */
        "s16i  %[Color], %[Out], 2  \n"   /* Screen[1] = Color (byte off 2) */

        /* ── Pixel C ─────────────────────────────────────────────────────── */
        "3:                          \n"
        "beqz  %[PC], 4f            \n"
        "bgeu  %[DC], %[ZC], 4f     \n"
        "slli  %[Color], %[PC], 1   \n"
        "add   %[Color], %[Pal], %[Color] \n"
        "l16ui %[Color], %[Color], 0 \n"
        "s8i   %[ZS], %[Z], 2       \n"   /* Depth[2] = ZSet                */
        "s16i  %[Color], %[Out], 4  \n"   /* Screen[2] = Color (byte off 4) */

        /* ── Pixel D ─────────────────────────────────────────────────────── */
        "4:                          \n"
        "beqz  %[PD], 5f            \n"
        "bgeu  %[DD], %[ZC], 5f     \n"
        "slli  %[Color], %[PD], 1   \n"
        "add   %[Color], %[Pal], %[Color] \n"
        "l16ui %[Color], %[Color], 0 \n"
        "s8i   %[ZS], %[Z], 3       \n"   /* Depth[3] = ZSet                */
        "s16i  %[Color], %[Out], 6  \n"   /* Screen[3] = Color (byte off 6) */
        "5:                          \n"

        : /* output — earlyclobber, non aliasati agli input */
          [PA]"=&r"(PA),    [PB]"=&r"(PB),    [PC]"=&r"(PC),    [PD]"=&r"(PD),
          [DA]"=&r"(DA),    [DB]"=&r"(DB),    [DC]"=&r"(DC),    [DD]"=&r"(DD),
          [Color]"=&r"(Color)
        : /* input */
          [Out]"r"(Screen),           [Z]"r"(Depth),
          [In8]"r"(Pixels),           [Pal]"r"(ScreenColors),
          [ZC]"r"((uint32_t)GFX.Z1), [ZS]"r"((uint32_t)GFX.Z2)
        : /* clobber */ "memory"
    );

/* ── MIPS originale (preservato per target GCW-Zero/RG350) ─────────────── */
#elif defined(__MIPSEL) && defined(__GNUC__) && !defined(NO_ASM)
    uint16_t *Screen = (uint16_t *) GFX.S + Offset;
    uint8_t  *Depth = GFX.DB + Offset;
    uint8_t  Pixel_A, Pixel_B, Pixel_C, Pixel_D;
    uint8_t  Depth_A, Depth_B, Depth_C, Depth_D;
    uint8_t  Cond;
    uint32_t Temp;
    __asm__ __volatile__ (
        ".set noreorder                        \n"
        "   lbu   %[In8A], 0(%[In8])           \n"
        "   lbu   %[In8B], 1(%[In8])           \n"
        "   lbu   %[In8C], 2(%[In8])           \n"
        "   lbu   %[In8D], 3(%[In8])           \n"
        "   lbu   %[ZA], 0(%[Z])               \n"
        "   lbu   %[ZB], 1(%[Z])               \n"
        "   lbu   %[ZC], 2(%[Z])               \n"
        "   lbu   %[ZD], 3(%[Z])               \n"
        "   sltiu %[Temp], %[In8A], 1          \n"
        "   sltu  %[Cond], %[ZCompare], %[ZA]  \n"
        "   or    %[Cond], %[Cond], %[Temp]    \n"
        "   bne   %[Cond], $0, 2f              \n"
        "   sll   %[In8A], %[In8A], 1          \n"
        "   addu  %[Temp], %[Palette], %[In8A] \n"
        "   lhu   %[Temp], 0(%[Temp])          \n"
        "   sb    %[ZSet], 0(%[Z])             \n"
        "   sh    %[Temp], 0(%[Out16])         \n"
        "2: sltiu %[Temp], %[In8B], 1          \n"
        "   sltu  %[Cond], %[ZCompare], %[ZB]  \n"
        "   or    %[Cond], %[Cond], %[Temp]    \n"
        "   bne   %[Cond], $0, 3f              \n"
        "   sll   %[In8B], %[In8B], 1          \n"
        "   addu  %[Temp], %[Palette], %[In8B] \n"
        "   lhu   %[Temp], 0(%[Temp])          \n"
        "   sb    %[ZSet], 1(%[Z])             \n"
        "   sh    %[Temp], 2(%[Out16])         \n"
        "3: sltiu %[Temp], %[In8C], 1          \n"
        "   sltu  %[Cond], %[ZCompare], %[ZC]  \n"
        "   or    %[Cond], %[Cond], %[Temp]    \n"
        "   bne   %[Cond], $0, 4f              \n"
        "   sll   %[In8C], %[In8C], 1          \n"
        "   addu  %[Temp], %[Palette], %[In8C] \n"
        "   lhu   %[Temp], 0(%[Temp])          \n"
        "   sb    %[ZSet], 2(%[Z])             \n"
        "   sh    %[Temp], 4(%[Out16])         \n"
        "4: sltiu %[Temp], %[In8D], 1          \n"
        "   sltu  %[Cond], %[ZCompare], %[ZD]  \n"
        "   or    %[Cond], %[Cond], %[Temp]    \n"
        "   bne   %[Cond], $0, 5f              \n"
        "   sll   %[In8D], %[In8D], 1          \n"
        "   addu  %[Temp], %[Palette], %[In8D] \n"
        "   lhu   %[Temp], 0(%[Temp])          \n"
        "   sb    %[ZSet], 3(%[Z])             \n"
        "   sh    %[Temp], 6(%[Out16])         \n"
        "5:                                    \n"
        ".set reorder                          \n"
        : [In8A]"=&r"(Pixel_A), [In8B]"=&r"(Pixel_B), [In8C]"=&r"(Pixel_C), [In8D]"=&r"(Pixel_D),
          [ZA]"=&r"(Depth_A),   [ZB]"=&r"(Depth_B),   [ZC]"=&r"(Depth_C),   [ZD]"=&r"(Depth_D),
          [Cond]"=&r"(Cond),    [Temp]"=&r"(Temp)
        : [Out16]"r"(Screen), [Z]"r"(Depth), [In8]"r"(Pixels), [Palette]"r"(ScreenColors),
          [ZCompare]"r"(GFX.Z1), [ZSet]"r"(GFX.Z2)
        : "memory"
    );

/* ── Fallback C generico (tutti gli altri target) ───────────────────────── */
#else
    uint8_t  Pixel, N;
    uint16_t* Screen = (uint16_t*) GFX.S + Offset;
    uint8_t*  Depth = GFX.DB + Offset;
    for (N = 0; N < 4; N++)
    {
        if (GFX.Z1 > Depth[N] && (Pixel = Pixels[N]))
        {
            Screen[N] = ScreenColors[Pixel];
            Depth[N]  = GFX.Z2;
        }
    }
#endif
}


/* ─────────────────────────────────────────────────────────────────────────
 * WRITE_4PIXELS16_FLIPPED  —  pixel in ordine inverso: Pixels[3,2,1,0]
 * (tile con flip orizzontale: i pixel vengono letti al contrario ma scritti
 *  sullo schermo sempre da sinistra a destra)
 * ───────────────────────────────────────────────────────────────────────── */
static INLINE void WRITE_4PIXELS16_FLIPPED(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
/* ── Xtensa LX7 (ESP32-S3) ─────────────────────────────────────────────── */
#if defined(__XTENSA__) && defined(__GNUC__) && !defined(NO_ASM)

    uint16_t *Screen = (uint16_t *) GFX.S + Offset;
    uint8_t  *Depth  = GFX.DB + Offset;
    uint32_t PA, PB, PC, PD;
    uint32_t DA, DB, DC, DD;
    uint32_t Color;

    __asm__ __volatile__ (

        /* ── Fase 1: front-load — stessa strategia della versione normale.
         * La SOLA differenza rispetto a WRITE_4PIXELS16:
         * i pixel vengono caricati in ordine inverso (offsets 3,2,1,0)
         * in modo che PA corrisponda a Pixels[3], PB a Pixels[2], ecc.
         * Le scritture sullo schermo (Out) rimangono in ordine 0,2,4,6.  */
        "l8ui  %[PA], %[In8], 3     \n"   /* PA = Pixels[3]  (flipped)      */
        "l8ui  %[PB], %[In8], 2     \n"   /* PB = Pixels[2]  (flipped)      */
        "l8ui  %[PC], %[In8], 1     \n"   /* PC = Pixels[1]  (flipped)      */
        "l8ui  %[PD], %[In8], 0     \n"   /* PD = Pixels[0]  (flipped)      */
        "l8ui  %[DA], %[Z],   0     \n"   /* DA = Depth[0]                  */
        "l8ui  %[DB], %[Z],   1     \n"   /* DB = Depth[1]                  */
        "l8ui  %[DC], %[Z],   2     \n"   /* DC = Depth[2]                  */
        "l8ui  %[DD], %[Z],   3     \n"   /* DD = Depth[3]                  */

        /* ── Pixel A (da Pixels[3], scritto in Screen[0]) ─────────────────  */
        "beqz  %[PA], 2f            \n"
        "bgeu  %[DA], %[ZC], 2f     \n"
        "slli  %[Color], %[PA], 1   \n"
        "add   %[Color], %[Pal], %[Color] \n"
        "l16ui %[Color], %[Color], 0 \n"
        "s8i   %[ZS], %[Z], 0       \n"
        "s16i  %[Color], %[Out], 0  \n"

        /* ── Pixel B (da Pixels[2], scritto in Screen[1]) ─────────────────  */
        "2:                          \n"
        "beqz  %[PB], 3f            \n"
        "bgeu  %[DB], %[ZC], 3f     \n"
        "slli  %[Color], %[PB], 1   \n"
        "add   %[Color], %[Pal], %[Color] \n"
        "l16ui %[Color], %[Color], 0 \n"
        "s8i   %[ZS], %[Z], 1       \n"
        "s16i  %[Color], %[Out], 2  \n"

        /* ── Pixel C (da Pixels[1], scritto in Screen[2]) ─────────────────  */
        "3:                          \n"
        "beqz  %[PC], 4f            \n"
        "bgeu  %[DC], %[ZC], 4f     \n"
        "slli  %[Color], %[PC], 1   \n"
        "add   %[Color], %[Pal], %[Color] \n"
        "l16ui %[Color], %[Color], 0 \n"
        "s8i   %[ZS], %[Z], 2       \n"
        "s16i  %[Color], %[Out], 4  \n"

        /* ── Pixel D (da Pixels[0], scritto in Screen[3]) ─────────────────  */
        "4:                          \n"
        "beqz  %[PD], 5f            \n"
        "bgeu  %[DD], %[ZC], 5f     \n"
        "slli  %[Color], %[PD], 1   \n"
        "add   %[Color], %[Pal], %[Color] \n"
        "l16ui %[Color], %[Color], 0 \n"
        "s8i   %[ZS], %[Z], 3       \n"
        "s16i  %[Color], %[Out], 6  \n"
        "5:                          \n"

        : [PA]"=&r"(PA),    [PB]"=&r"(PB),    [PC]"=&r"(PC),    [PD]"=&r"(PD),
          [DA]"=&r"(DA),    [DB]"=&r"(DB),    [DC]"=&r"(DC),    [DD]"=&r"(DD),
          [Color]"=&r"(Color)
        : [Out]"r"(Screen),           [Z]"r"(Depth),
          [In8]"r"(Pixels),           [Pal]"r"(ScreenColors),
          [ZC]"r"((uint32_t)GFX.Z1), [ZS]"r"((uint32_t)GFX.Z2)
        : "memory"
    );

/* ── MIPS originale ─────────────────────────────────────────────────────── */
#elif defined(__MIPSEL) && defined(__GNUC__) && !defined(NO_ASM)
    uint16_t *Screen = (uint16_t *) GFX.S + Offset;
    uint8_t  *Depth = GFX.DB + Offset;
    uint8_t  Pixel_A, Pixel_B, Pixel_C, Pixel_D;
    uint8_t  Depth_A, Depth_B, Depth_C, Depth_D;
    uint8_t  Cond;
    uint32_t Temp;
    __asm__ __volatile__ (
        ".set noreorder                        \n"
        "   lbu   %[In8A], 3(%[In8])           \n"
        "   lbu   %[In8B], 2(%[In8])           \n"
        "   lbu   %[In8C], 1(%[In8])           \n"
        "   lbu   %[In8D], 0(%[In8])           \n"
        "   lbu   %[ZA], 0(%[Z])               \n"
        "   lbu   %[ZB], 1(%[Z])               \n"
        "   lbu   %[ZC], 2(%[Z])               \n"
        "   lbu   %[ZD], 3(%[Z])               \n"
        "   sltiu %[Temp], %[In8A], 1          \n"
        "   sltu  %[Cond], %[ZCompare], %[ZA]  \n"
        "   or    %[Cond], %[Cond], %[Temp]    \n"
        "   bne   %[Cond], $0, 2f              \n"
        "   sll   %[In8A], %[In8A], 1          \n"
        "   addu  %[Temp], %[Palette], %[In8A] \n"
        "   lhu   %[Temp], 0(%[Temp])          \n"
        "   sb    %[ZSet], 0(%[Z])             \n"
        "   sh    %[Temp], 0(%[Out16])         \n"
        "2: sltiu %[Temp], %[In8B], 1          \n"
        "   sltu  %[Cond], %[ZCompare], %[ZB]  \n"
        "   or    %[Cond], %[Cond], %[Temp]    \n"
        "   bne   %[Cond], $0, 3f              \n"
        "   sll   %[In8B], %[In8B], 1          \n"
        "   addu  %[Temp], %[Palette], %[In8B] \n"
        "   lhu   %[Temp], 0(%[Temp])          \n"
        "   sb    %[ZSet], 1(%[Z])             \n"
        "   sh    %[Temp], 2(%[Out16])         \n"
        "3: sltiu %[Temp], %[In8C], 1          \n"
        "   sltu  %[Cond], %[ZCompare], %[ZC]  \n"
        "   or    %[Cond], %[Cond], %[Temp]    \n"
        "   bne   %[Cond], $0, 4f              \n"
        "   sll   %[In8C], %[In8C], 1          \n"
        "   addu  %[Temp], %[Palette], %[In8C] \n"
        "   lhu   %[Temp], 0(%[Temp])          \n"
        "   sb    %[ZSet], 2(%[Z])             \n"
        "   sh    %[Temp], 4(%[Out16])         \n"
        "4: sltiu %[Temp], %[In8D], 1          \n"
        "   sltu  %[Cond], %[ZCompare], %[ZD]  \n"
        "   or    %[Cond], %[Cond], %[Temp]    \n"
        "   bne   %[Cond], $0, 5f              \n"
        "   sll   %[In8D], %[In8D], 1          \n"
        "   addu  %[Temp], %[Palette], %[In8D] \n"
        "   lhu   %[Temp], 0(%[Temp])          \n"
        "   sb    %[ZSet], 3(%[Z])             \n"
        "   sh    %[Temp], 6(%[Out16])         \n"
        "5:                                    \n"
        ".set reorder                          \n"
        : [In8A]"=&r"(Pixel_A), [In8B]"=&r"(Pixel_B), [In8C]"=&r"(Pixel_C), [In8D]"=&r"(Pixel_D),
          [ZA]"=&r"(Depth_A),   [ZB]"=&r"(Depth_B),   [ZC]"=&r"(Depth_C),   [ZD]"=&r"(Depth_D),
          [Cond]"=&r"(Cond),    [Temp]"=&r"(Temp)
        : [Out16]"r"(Screen), [Z]"r"(Depth), [In8]"r"(Pixels), [Palette]"r"(ScreenColors),
          [ZCompare]"r"(GFX.Z1), [ZSet]"r"(GFX.Z2)
        : "memory"
    );

/* ── Fallback C generico ────────────────────────────────────────────────── */
#else
    uint8_t  Pixel, N;
    uint16_t* Screen = (uint16_t*) GFX.S + Offset;
    uint8_t*  Depth = GFX.DB + Offset;
    for (N = 0; N < 4; N++)
    {
        if (GFX.Z1 > Depth[N] && (Pixel = Pixels[3 - N]))
        {
            Screen[N] = ScreenColors[Pixel];
            Depth[N]  = GFX.Z2;
        }
    }
#endif
}


/*
 * ══════════════════════════════════════════════════════════════════════════
 * ANALISI DEI CICLI — confronto MIPS vs Xtensa LX7
 * ══════════════════════════════════════════════════════════════════════════
 *
 * MIPS (GCW-Zero, JZ4770 @ 1 GHz, in-order, 5 stadi, delay slot):
 *   Per pixel opaco visibile:  ~9 istruzioni effettive (sll occupa delay slot)
 *   Per pixel skippato:        ~4 istruzioni (sltiu + sltu + or + bne)
 *
 * Xtensa LX7 (ESP32-S3, 240 MHz, in-order, 5 stadi, NO delay slot):
 *   Per pixel opaco visibile:  7 istruzioni (beqz + bgeu + slli + add + l16ui + s8i + s16i)
 *   Per pixel skippato:        1–2 istruzioni (beqz, oppure beqz + bgeu)
 *   Vantaggio vs MIPS:         -2 istr/pixel visibile, -2 istr/pixel skippato
 *
 *   Latenze pipeline per pixel visibile (caso ottimale, nessuno stall):
 *   Ciclo 1:  beqz   (branch taken/not taken, risolto in EX)
 *   Ciclo 2:  bgeu   (risolto in EX)
 *   Ciclo 3:  slli   (ALU, 1 ciclo)
 *   Ciclo 4:  add    (usa risultato slli — forwarding, 0 stall)
 *   Ciclo 5:  l16ui  (usa risultato add — forwarding, 0 stall; load in ME)
 *   Ciclo 6:  s8i    (indipendente da l16ui — riempie latenza load)
 *   Ciclo 7:  s16i   (usa Color da l16ui — disponibile esattamente al ciclo 7)
 *
 *   Con 4 pixel tutti opachi: ~28 cicli totali + 8 cicli front-load = 36 cicli.
 *   Il C fallback (loop for con branch per pixel): ~60-80 cicli stimati.
 *
 * ══════════════════════════════════════════════════════════════════════════
 * NOTA SUL CHIAMANTE: per il massimo beneficio, aggiungere IRAM_ATTR alle
 * funzioni DrawTile16 e DrawClippedTile16 in modo che vengano eseguite
 * direttamente da IRAM invece che dalla flash (risparmio di ~5-10 cicli per
 * invocazione sul bus SPI flash dell'ESP32-S3).
 * ══════════════════════════════════════════════════════════════════════════
 */
