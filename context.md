### **1. Game Overview & Objective**

- **Core Concept:** A fast-paced, competitive educational game where two players race to figure out how many smaller "rep-tiles" fit together to make a larger version of that same shape.
- **Player Count:** 2 Players — **Blue** vs. **Red**.
- **Game Length:** 3 rounds per game, with shapes getting progressively more difficult (Round 1 = Easy, Round 2 = Medium, Round 3 = Hard).
- **Time Limit:** Configurable per game from the web dashboard (5–600 seconds). Default is 60 seconds. Players can also end a round early — see the gameplay loop below.
- **Scoring (per round):**
  - **+10 to the fastest** player to reach a correct placement and still be correct at round end.
  - **+5 to the second** player to reach a correct placement and still be correct at round end.
  - **−5 per wrong tag** in the player's final placement (any tag on a reader that is not a valid answer).
  - A player who is not correct at the end gets no speed bonus, regardless of whether they were correct mid-round.
- **What counts as "correct":** Each shape has one or more **valid answer readers**. Placing *any one* of the player's tokens on a valid reader counts as a correct answer; covering additional valid readers is a free bonus (no extra points, no penalty). The system silently records the first time each player reaches a correct placement and uses that timestamp to decide who was fastest, but the final answer is whatever the player has on the board when the round ends.
- **Winning:** The player with the highest total score after Round 3 wins. Ties are recognised explicitly.

---

### **2. Hardware & Physical Setup**

- **The Play Mats:** Each player has a dedicated workspace on their side of the game to physically manipulate the blank, untagged laser-cut shapes.
- **The Central Wall:** A divider separating the players, housing the NFC readers, slots, LEDs, and buzzers. Individual white LEDs sit above each reader to highlight the correct answers at the end of a round.
- **The Slots / Readers:** 10 PN532 NFC readers wired to the ESP32, labelled 1–10, shared between the two players (each reader can read two tags simultaneously — one from each side of the wall).
  - **Reader 1** is the dedicated "Not a Rep-Tile / None" answer slot. Any shape whose answer is "no rep-tiles fit" maps to Reader 1.
  - **Reader 10** is faulty. None of the assigned shapes use Reader 10 anyway.
- **Answer Tokens:** Each player uses a set of NFC-tagged tokens in their colour:
  - **Blue** player uses Blue-coloured tokens.
  - **Red** player uses Red-coloured tokens.
  - Tokens are programmed via a **WRITE MODE** in the dashboard which records the player role (Blue or Red) on the tag itself, so the system always knows who placed which token.
- **The Display:** Two 20×4 character LCD screens (I²C, address `0x27`) facing both players. They share the same I²C bus and mirror each other, displaying the timer, current round, target shape, and live scores. Long shape names automatically wrap to a second line.
- **The Buzzer:** A piezo buzzer plays distinct tones for tag placement feedback, round start (3-2-1-GO), round end (klaxon), and victory / tie tunes on game over. **There is no audio cue during the round when a player reaches a correct placement** — speed tracking is silent so neither player knows in real time how the other is doing.
- **The Button:** A single hardware button drives game flow.
  - **Short press:** advances state (start game → start round → next round → restart).
  - **Short press during a round:** acts as a "player ready" signal. The first press shows `PLAYER READY (1 of 2)`; the second press ends the round immediately, with the final scoring computed from whatever is on the board at that moment.
  - **Short press during the round-results screen:** skips the post-clear countdown and advances to the next round / game-over — but only once all tokens have been physically removed from the board. It is ignored while `Remove tokens!` is showing.
  - **Long press (3 s hold):** resets the game from any state back to the home screen.

---

### **3. Software & Organiser Dashboard**

- **Connection:** The ESP32 hosts a WiFi access point (`SSID: REP-TILE`, `password: notareptile`). The organiser connects to it via 192.168.1.4.
- **Shape Database:** The organiser can add or delete shapes. Each shape stores:
  - A **name** (e.g. "Red Triangle").
  - A **reader mask** — the set of readers (1–10) that are valid answers for this shape. A shape with no readers ticked defaults to **Reader 1** (the "not a rep-tile" answer). Placing a token on *any one* of the masked readers counts as the correct answer; the mask does **not** mean "all of these must be covered".
  - A **difficulty** of Easy (1), Medium (2), or Hard (3).
  - Defaults are flashed on first boot and survive reboots; bumping the internal `SHAPES_VERSION` reloads the defaults.
- **Round Selection:** Two modes, toggled in the Round Configuration panel.
  - **Random:** the system auto-picks one Easy shape for Round 1, one Medium for Round 2, one Hard for Round 3.
  - **Manual:** the organiser picks each round from a dropdown — but the dropdowns are filtered by difficulty (Round 1 only lists Easy shapes, Round 2 only Medium, Round 3 only Hard), and the server rejects any submission that violates this rule.
- **Round Timer:** A numeric input in the Round Configuration panel sets the per-round duration (5–600 seconds). The change applies to the next round started.
- **Upcoming View:** Shows the three shapes assigned to the current game (with their difficulties) and live Blue / Red scores. Updates every 2 s while the game is in progress.
- **Reader Cards:** A live grid of all 10 readers showing what tag (if any) is in each of the two slots, including the role of the tag (`(BLUE)`, `(RED)`, or `(UNASSIGNED)`).
- **Write Mode:** A toggle that switches the system from gameplay/read mode into tag-programming mode. The organiser picks Blue or Red, then taps each blank token on any reader to imprint that role.

---

### **4. The Gameplay Loop (Step-by-Step)**

1. **Home / PREP State:** LCD shows `REP-TILE READY / Press to start`. While in this state, placing a tag on any reader plays a beep and shows `Reader X detected a tag!` — useful for verifying reader wiring and tag programming before a game.
2. **Game Setup:** Organiser presses the button. The system either auto-picks shapes by difficulty (random mode) or uses the dashboard-selected ones (manual mode). The LCD shows all three round assignments at once so the organiser can grab the matching bags of tiles.
3. **Round Intro:** Organiser presses the button again. LCD shows `ROUND <n> / Shape: <name> / Press to start`. Long names wrap to the next line.
4. **Round Start:** Organiser presses the button a third time. The buzzer plays a `3-2-1-GO` countdown and the round timer begins. The LCD shows `TIME: Xs / (Next: <letter>) ROUND <n>: / <shape name> / Blue: X Red: Y` and refreshes once per second.
5. **The Puzzle:** Players physically arrange their blank tiles on their mats to discover the correct rep-tile reader number(s) for the current shape.
6. **Submission:** Each player slots their coloured tokens into the corresponding numbered slots on their side of the wall. Only one valid placement is required to be "correct" — additional valid placements are free bonus coverage, while placements on non-valid readers will cost the player points at the end of the round.
7. **Silent Speed Tracking:** Behind the scenes the system records the first moment each player has any one of the valid readers covered with their colour. Nothing is shown or sounded — the players cannot tell from the device whether/when they got it right, or who got it first.
8. **End of Round — Trigger:** A round ends in exactly one of two ways:
   - The round timer hits 0.
   - Both players press the shared button (`PLAYER READY (1 of 2)` then immediate end on the second press).
9. **End of Round — Finalisation & Scoring:** At the end-of-round moment the system:
   - Looks at each player's current board state. A player is "correct" if at least one of their tags is on a valid reader.
   - Awards **+10** to whichever correct player reached their first-correct state earliest, **+5** to the other correct player (if any).
   - Subtracts **−5 per wrong-reader placement** in each player's current state.
   - Applies the resulting `pointsThisRound` to each player's running total.
10. **Round Results Screen (SCORING):** LCD displays the result, with the line layout:
    ```
    BLUE WAS FASTEST!         (or RED WAS FASTEST! / NO ONE FINISHED)
    Round B:+10 R:-5
    Total B:10 R:5
    Remove tokens!            (or "Continuing in Xs..." once the board is clear)
    ```
    The buzzer plays a klaxon as this screen appears.
11. **Token-Clear Gate:** While *any* token is still detected on *any* reader, line 4 reads `Remove tokens!` and the screen **will not advance** — neither the button nor any timer will move it on. RF fields stay live so removals are detected in near real time (a brief hysteresis delay after physically lifting a token).
12. **Post-Clear Countdown:** The instant the system sees no tokens on the board, line 4 switches to `Continuing in 5s...` and counts down 5 → 1. A short press of the button during this window advances immediately. If a token is placed back on during the countdown, the screen reverts to `Remove tokens!` and the countdown restarts the next time the board is cleared.
13. **Between Rounds (Rounds 1 → 2 and 2 → 3):** When the countdown expires (or the button is pressed), the LCD jumps to the next Round Intro: `ROUND <n+1> / Shape: <name> / Press to start`. The organiser presses the button again to begin that round.
14. **After Round 3 SCORING — Straight to End Screen:** Once the Round 3 results screen passes the token-clear gate and its countdown, the system goes directly to the Game Over screen — there is no extra intermediate step.
15. **Game Over:** The buzzer plays the victory or tie tune. The LCD shows:
    ```
    GAME OVER
    BLUE WINS!                (or RED WINS! / IT'S A TIE!)
    Blue: X  Red: Y
    Restart in Ns
    ```
    The countdown runs from 10 → 1 (so this is a 10-second hold, longer than the inter-round 5-second hold). A short press of the button skips the rest of the countdown.
16. **Auto-Restart:** When the Game Over countdown elapses (or the button is pressed), the system automatically returns to the **PREP** home screen. Scores, finish-order timestamps, and the active round shapes are all reset. The organiser must press the button again to start a fresh game (which will pick fresh shapes if Random mode is selected).
17. **Hard Reset (Any State):** A 3-second long press of the button resets the game from any state back to PREP immediately.

---
