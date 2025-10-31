/* =============================================================================
 * MUUTUJATE / VÄLJADE KIIRVIIDE (selgitused)
 * -----------------------------------------------------------------------------
 * Legend:
 *  - Nimi:   muutuja/valdkond/define nimi koodis
 *  - Tüüp:   C-tüüp (või enum)
 *  - Ulatus: "ctx väli" = kontekstistruktuuri väli; "lok." = lokaalne muutuja;
 *            "param" = funktsiooni parameeter; "define" = eeltöötlus define
 *  - Selgitus: mida väärtus tähendab ja kuidas kasutatakse
 *  - Vaikimisi: väärtus pärast init/reset
 * ========================================================================== */

 /*
  * --------------------------- PÕHIKONTEKST ---------------------------
  * Struktuur: struct aj_switch_ctx_s (alias aj_switch_ctx_t)
  *
  * | Nimi              | Tüüp                   | Ulatus    | Selgitus                                                                 | Vaikimisi |
  * |-------------------|------------------------|-----------|--------------------------------------------------------------------------|-----------|
  * | enabled           | uint8_t                | ctx väli  | Lüliti olek: 0 = OFF, 1 = ON.                                           | 0         |
  * | mode              | aj_switch_mode_t       | ctx väli  | Režiim: AJ_SWITCH_MODE_AUTO / LOW / HIGH.                                | AUTO      |
  * | controller_only   | uint8_t                | ctx väli  | Kui 1, siis LOKAALNE režiimimuutus on keelatud (enable võib jääda lubatuks, |
  * |                   |                        |           | kui makroga pole määratud teisiti).                                      | 0         |
  * | last_change_ms    | aj_timestamp_ms_t      | ctx väli  | Viimase edukalt rakendatud muutuse ajatempel millisekundites.            | 0         |
  * | notify_cb         | aj_switch_notify_cb_t  | ctx väli  | Callback, mida kutsutakse muutuse korral (enabled, mode, timestamp, usr).| NULL      |
  * | notify_ctx        | void*                  | ctx väli  | Kasutaja kontekst, antakse notify_cb-le edasi.                           | NULL      |
  *
  * Märkus: aj_switch_reset() säilitab notify_cb ja notify_ctx, kõik muu nullitakse.
  */

 /*
  * ------------------------------ ENUM-id ------------------------------
  *
  * aj_switch_mode_t:
  *   - AJ_SWITCH_MODE_AUTO  : Automaatrežiim
  *   - AJ_SWITCH_MODE_LOW   : Madal (LOW)
  *   - AJ_SWITCH_MODE_HIGH  : Kõrge (HIGH)
  *
  * aj_switch_result_t:
  *   - AJ_SW_RET_OK         : Muutus rakendus
  *   - AJ_SW_RET_NOCHANGE   : Midagi ei muutunud (sama olek või debounce/blokeering)
  *   - AJ_SW_RET_INVALID    : Vigane sisend/kontext (nt ctx NULL, vigane mode)
  *   - AJ_SW_RET_DENIED     : Tegevus keelatud poliitika tõttu (nt controller_only lokaalsel muutmisel)
  */

 /*
  * ------------------------------ DEFINE-id ------------------------------
  *
  * | Nimi                                      | Tüüp    | Ulatus  | Selgitus                                                                                   | Vaikimisi |
  * |-------------------------------------------|---------|---------|--------------------------------------------------------------------------------------------|-----------|
  * | AJ_SWITCH_MIN_MS_BETWEEN_CHANGES          | uint    | define  | Debounce – min. aeg ms kahe järjestikuse muudatuse vahel. 0 = debouncet pole.             | 0         |
  * | AJ_SWITCH_DENY_LOCAL_ENABLE_WHEN_CONTROLLER_ONLY | flag | define  | Kui defineeritud, siis controller_only=1 korral keelatakse ka LOKAALNE enable/disable.    | (määramata)|
  */

 /*
  * --------------------------- FUNKTSIOONI PARAMEETRID ---------------------------
  *
  * | Nimi          | Tüüp                | Kus esineb                                  | Selgitus                                  |
  * |---------------|---------------------|---------------------------------------------|-------------------------------------------|
  * | buffer        | void*               | aj_switch_init                              | Väljast ette antud mäluplokk kontekstile. |
  * | buffer_size   | size_t              | aj_switch_init                              | buffer suurus baitides.                   |
  * | ctx           | aj_switch_ctx_t*    | enamik API-sid                               | Kontekst, millel operatsioon toimub.      |
  * | when_ms       | aj_timestamp_ms_t   | set*/apply*/request* funktsioonid           | Muutushetke ajatempel ms (debounce jaoks). |
  * | enable        | uint8_t             | set/request/pack/unpack                     | 0/1 lüliti olek.                           |
  * | mode          | aj_switch_mode_t    | set_mode_*, pack/unpack                     | Soovitud režiim.                           |
  * | cmd           | uint8_t             | aj_switch_apply_cmd_from_controller, unpack | Pakitud 1-baidine käsk ELRS-ist.          |
  * | cb            | aj_switch_notify_cb_t | aj_switch_register_notify_cb               | Muutuseteavituse callback.                |
  * | user_ctx      | void*               | aj_switch_register_notify_cb                | Kasutaja kontekst callbackile.            |
  */

 /*
  * ------------------------------ LOKAALSED ABID ------------------------------
  *
  * | Nimi        | Tüüp               | Kus         | Selgitus                                                                         |
  * |-------------|--------------------|-------------|----------------------------------------------------------------------------------|
  * | delta       | aj_timestamp_ms_t  | aj_can_apply_now (tingimusel, et debounce > 0) | when_ms - last_change_ms, kasutatakse debouncimiseks. |
  * | m           | uint8_t            | cmd_pack/unpack | Režiimi kood bitiväljas (0=AUTO,1=LOW,2=HIGH).                               |
  * | en          | uint8_t            | apply_cmd_from_controller | Lahtipakitud enable bit.                                      |
  * | changed     | uint8_t            | apply_cmd_from_controller | Lipp, kas olek (enable/mode) tegelikult muutus.               |
  */

 /*
  * --------------------------- PAKITUD KÄSU BITIVÄLI ---------------------------
  *
  * aj_cmd (uint8_t) paigutus:
  *   bit0     : enable (0 = OFF, 1 = ON)
  *   bit1..2  : mode   (00 = AUTO, 01 = LOW, 10 = HIGH, 11 = AUTO (tolerantne fallback))
  *   bit3..7  : (pole kasutusel)
  *
  * Pakendamine:
  *   aj_switch_cmd_pack(enable, mode) -> uint8_t
  *
  * Lahtipakkimine:
  *   aj_switch_cmd_unpack(cmd, &out_enable, &out_mode)
  */

 /*
  * --------------------------- KONTROLLER vs LOKAAL ---------------------------
  *
  * - aj_switch_set_mode_local(...)     : Keelatakse, kui controller_only = 1 (tagastab DENIED).
  * - aj_switch_set_enabled(...)        : Lubatud ka siis, kui controller_only = 1,
  *                                       KUI EI ole defineeritud AJ_SWITCH_DENY_LOCAL_ENABLE_WHEN_CONTROLLER_ONLY.
  * - aj_switch_set_mode_from_controller(...), aj_switch_request_enable_from_controller(...)
  *                                     : Lubatud alati (mööduvad controller_only lokaalsest keelust).
  *
  * Debounce:
  * - Kõik muutused (nii lokaalsed kui kontrollerist) austavad AJ_SWITCH_MIN_MS_BETWEEN_CHANGES,
  *   v.a kui väärtus ei muutu (tagastatakse NOCHANGE ja callbacki ei kutsuta).
  *
  * Callback:
  * - notify_cb kutsutakse ainult siis, kui olek muutus (enabled või mode) ja muudatus rakendati.
  */

 /*
  * --------------------------- TX/RX KANALITE MÄPP ---------------------------
  *
  * TX (näide):
  *   enable = (CH4 > 0)
  *   mode   = (CH5 < -33 ? LOW : CH5 > 33 ? HIGH : AUTO)
  *   cmd    = aj_switch_cmd_pack(enable, mode)
  *
  * RX:
  *   aj_switch_apply_cmd_from_controller(ctx, cmd, now_ms)
  *   -> rakendab (enable, mode) korraga ja väljastab notify_cb, kui midagi muutus.
  */
/* ============================================================================= */