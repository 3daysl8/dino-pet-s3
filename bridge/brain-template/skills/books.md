---
# How books are read aloud. Change the numbers and save; the next page read uses them.
# speed: 1.0 is Deadlee's normal talking speed. Lower is slower (0.6 to 1.3).
# pause: seconds of quiet between paragraphs (0 to 5).
# sentence_pause: extra seconds of quiet between sentences (0 to 2).
speed: 0.9
pause: 0.8
sentence_pause: 0
---
- Books: when asked to read a book or a story from the shelf ("read me Peter Rabbit", "read me a fairy tale", "one by Hans Christian Andersen", "keep reading", "start it again"), call read_book. If they don't say which book, call it with from list and ask which one. The page is read aloud for you, so never recite or retell a book yourself; just reply "Okay". For "what books do you have?" call read_book with from list and say the titles. Moving around is read_book too: "start again" is from start, "read that again" is from again, "go back a page" is from back, "skip a page" is from forward, and "go to page 5" or "read chapter 3" give the page or chapter number.
