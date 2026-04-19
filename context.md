### **1. Game Overview & Objective**

- **Core Concept:** A fast-paced, competitive educational game where two players race to figure out how many smaller "rep-tiles" fit together to make a larger version of that same shape.
- **Player Count:** 2 Players (Player 1 vs. Player 2).
- **Game Length:** 3 rounds per game, with shapes getting progressively more difficult.
- **Time Limit:** 5 seconds per round FOR TESTING, 90 seconds in the real thing
- **Winning:** The player with the most points at the end of round 3 wins.

---

### **2. Hardware & Physical Setup**

- **The Play Mats:** Each player has a dedicated workspace on their side of the game to physically manipulate the blank, untagged laser-cut shapes.
- **The Central Wall:** A divider separating the players, housing the NFC readers, slots, LEDs, and buzzers. Individual white LEDs sit above each reader to highlight the correct answers at the end of a round.
- **The Slots:** 9 dedicated submission slots on each player's side of the wall (Numbers 2 through 10). The readers are shared between players, and are already configured in the libraries to read two tags at once accurately.
- **Answer Tokens:** Players use a set of dedicated NFC-tagged answer tokens.
  - Player 1 uses a distinct color (e.g., Red tokens).
  - Player 2 uses a distinct color (e.g., Blue tokens).
  - The system can enter a write mode via the web dashboard and write to tags whether they belong to player 1/2. The system will detect this accurately for scoring.
- **The Display:** Two OLED screens facing both the players, displaying the timer, current round, required shapes, and total points. They mirror each other, displaying the exact same things.
- **The Reset Mechanism:** A motorized or linear-actuated bar at the bottom of the wall. When pulled, it opens the bottom of the slots, dropping all submitted tokens into a return tray for quick reset.

---

### **3. Software & Organizer Dashboard**

- **Web Dashboard:** A control centre for the game organiser.
- **Shape Database:** The organiser can add new shapes to the system via the dashboard. Each shape should have the option to be assigned readers that it is valid for. For example, ‘red triangle’ assigned to readers 1 and 2.
- **Round Selection:** The organiser can manually choose the shape selection from the database for round 1, 2 and 3, or the system can randomly decide between shapes, ensuring that difficulty increases from rounds 1 to 3 (so always ensure round 1 is easy, round 2 is medium, round 3 is hard shapes). This will be determined by the ‘difficulty’ property set with the shape when it is entered into the database.
- **Upcoming View:** Displays exactly which shapes are needed for the upcoming game so the organizer can prep the physical bags of tiles. It displays all the three random shapes it has selected and which rounds they have been assigned to at the very beginning, and before each actual round it will say the shape designated to that round, for example ‘Round 1: Red Triangle’.

---

### **4. The Gameplay Loop (Step-by-Step)**

1. **Preparation:** The game assigns the shapes for the 3 rounds. The organiser views one of the OLED, grabs the corresponding 3 bags of shapes, and keeps them ready the players.
2. **Game Start:** The organiser presses the main hardware button to initialise the game.
3. **Round Intro:** Both OLED screens announces "Round 1" and displays the target shape.
4. **Round Start:** The organiser presses the button a second time. The 90-second countdown begins on the OLEDs.
5. **The Puzzle:** Players scramble to physically arrange their blank tiles on their mats to discover the correct rep-tile numbers (for red triangle, it could be 2, 4, 8 so the correct answers would be readers 2/4/8).
6. **Submission:** Once a player figures out an answer, they grab a token from their set of coloured Answer Tokens (each player has their own set) and slots it into the corresponding number on their side of the central wall.
7. **Immediate Feedback:** The wall registers the drop with a localised LED flash and a buzzer sound. Players can submit multiple tokens if they find multiple solutions.
8. **Time's Up:** At 0 seconds, the round freezes. The PN532 readers are assessed to verify the submitted tokens.
9. **Scoring:** The correct LEDs light up above their corresponding readers, Scores are updated on the OLEDs (10 points per correct token, -5 for incorrect ones to avoid spamming tokens).
10. **The Reset:** The mechanical bar automatically pulls to drop and return the Answer Tokens.
11. **Next Round:** The system waits for the organizer's button press to begin Round 2. (Rinse and repeat for 3 rounds).
12. **Game Over:** A celebratory tune plays on the buzzer, and the OLED screens declares the ultimate winner.

---

### **5. Immersive Theme Ideas**

To elevate the game from a "math test" to an immersive experience, wrap the physical design, LED colors, and buzzer sounds into one of these narratives:

- **Space Station Reactor Repair:** Players are engineers assembling "Plasma Cores." Neon blue/green LEDs and sci-fi scanning/booping sound effects.
- **The Alchemist's Vault:** Players are wizards combining geometric "Crystals." Purple/gold LEDs and magical, mystical chime sound effects.
- **The Robot Assembly Line:** Players are mechanics building bot-armor. Hazard-striped wall, industrial orange/red LEDs, and mechanical clanking sound effects.
