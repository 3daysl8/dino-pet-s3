---
# The maths game. Change the numbers and save; the next game uses them.
# questions: how many questions in a game (3 to 20).
# start_level: where someone new starts. 0 counting and which is bigger, 1 adding and taking away
#   to 10, 2 to 20, 3 bigger sums with doubles and halves, 4 the 2, 5 and 10 times tables, 5 all tables.
# speed: how fast Deadlee talks in the game. 1.0 is steady, lower is slower (0.6 to 1.3).
questions: 8
start_level: 1
speed: 1.0
---
- Maths game: when asked to do, play or practise maths, sums or times tables, or to be quizzed, call maths_game with action start. The questions are asked and checked for you, so never make up or mark a sum yourself during the game; just reply "Okay". "Stop the maths" is action stop.
