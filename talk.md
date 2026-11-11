How Not to Trust Your Microcontroller
Defensive engineering for devices that fail when you're not looking

The CCC audience will probably appreciate that the underlying subject isn't really LEDs, ESP8266s, or Sming. It's epistemology under hostile conditions:

    How do you know what your tiny computer actually did when you weren't there?

The opening

I'd start with the classic embedded promise:

    "It's just a little Wi-Fi device. What could possibly go wrong?"

Then:

Everything.

    Wi-Fi disappears.

    DHCP says you're connected when you're not.

    lwIP runs out of buffers.

    heap fragmentation accumulates for weeks.

    the filesystem is halfway through a write when the power disappears.

    the OTA update dies halfway through.

    mDNS gives you half the information you need.

    the compiler does something surprising.

    the device crashes at 03:17.

    and then reboots.

And your developer is sitting 500 km away saying:

    "Well... shit. What happened?"

That's your hook.
Act I — The crime scene

I'd introduce the Lightinator system first.

Not as "look at this cool logging server", but as a question:

    What evidence survives when the victim is a €5 microcontroller with 80 KB of usable RAM?

Then reveal the architecture:

             ┌──────────────┐
             │ ESP8266      │
             │              │
             │ application  │
             └──────┬───────┘
                    │
              compressed logs
                    │
                    ▼
             ┌──────────────┐
             │ UDP/lwIP     │
             └──────┬───────┘
                    │
                    ▼
             ┌──────────────┐
             │ Lightinator  │
             │              │
             │ evidence     │
             └──────────────┘

Then:

    "Notice what isn't in this architecture: hope."

That could get a laugh.
Act II — The debugging story

Then tell the Sming/lwIP story.

This is where I think the talk gets really good.

Start with the symptom:

    "TCP says it succeeded."

Then:

    "The application says the data never arrived."

And walk through the investigation.

Not the final fix first.

Show the progression:

symptom
   ↓
wrong hypothesis
   ↓
more instrumentation
   ↓
framework
   ↓
lwIP
   ↓
packet capture
   ↓
source inspection
   ↓
"wait, that's impossible..."
   ↓
actual mechanism

The lesson:

    Debugging is the process of removing things you merely believe.

That is very CCC-compatible.

You can make a broader point:
Abstractions are hypotheses.

send() doesn't mean "the packet arrived."

connected() doesn't mean "the network works."

GotIP doesn't necessarily mean "the network stack is ready for what I want to do."

file.exists() doesn't mean "the file is valid."

malloc() succeeding doesn't mean "I have enough memory to finish this operation."

And:

OTA succeeded doesn't mean "the device will boot tomorrow."
Act III — The device that investigates its own death

Now bring in the Huffman logger.

This is where I'd show the audience the rather ridiculous-looking collection of components:

HuffmanEncoder
HuffmanDecoder
HuffmanRingBuffer
udpSyslogStream
MultiOutputStream
Lightinator

Then explain:

    "Someone built a custom compression scheme because their logging system was itself a resource-constrained embedded application."

The important punchline:

The logging system is defensive against the logging system.

If RAM gets low:

application > logging

If the network gets congested:

buffer > packet storm

If the buffer fills:

new evidence > old evidence

That is a beautiful principle to expose:

    Diagnostics must fail less dangerously than the thing they're diagnosing.

Act IV — The OTA suicide pact

Then move into OTA.

I'd make this section theatrical.

Put this on screen:

DOWNLOAD FIRMWARE
       ↓
     REBOOT
       ↓
      💀

Then:

    "This is not an OTA strategy."

Then reveal what the actual system does.
Web application

ACTIVE
  │
  ├── download
  ▼
STAGING
  │
  ├── verify
  ▼
COMMIT
  │
  ▼
ACTIVE'

And ask:

    "What happens if I pull the power at every arrow?"

That's the entire chapter in one question.

Then discuss:

    persistent OTA state;

    interrupted downloads;

    MD5 verification;

    staging;

    insufficient filesystem space;

    retry;

    reboot recovery;

    boot-partition handling;

    crash loops.

The key lesson:

    An OTA implementation isn't an update mechanism. It's a distributed transaction with a power switch controlled by the user.

That's a line I'd definitely put in the talk.
Act V — Everything is a distributed system

Now mDNS and controller discovery.

Ask:

    "What happens when eight microcontrollers try to figure out who exists?"

Then:

A record arrives
TXT record arrives later
another device disappears
leader changes
packet gets lost
cache expires

And show the Controllers / mDNSHandler architecture.

The audience gets the realization:

    Congratulations. Your collection of €5 Wi-Fi lightbulbs is now a distributed system.

Then introduce:

    stale information;

    partial information;

    eventual consistency;

    deterministic leader election;

    timeouts;

    bounded membership;

    resource exhaustion.

This is an excellent bridge from embedded programming into systems engineering.
Act VI — The really uncomfortable question

At this point I'd stop talking about this particular project.

Put up:

    How much do you actually know about your embedded system?

Then list:

What did it receive?
What did it send?
How much heap did it have?
Which task was running?
What was the TCP state?
Which firmware was active?
Was the filesystem consistent?
Why did it reboot?
What happened immediately before?

And then:

    "If your answer to those questions is 'I don't know', that's not a debugging problem."

Pause.

    "That's an architecture problem."

That's the central thesis of the talk.
Act VII — Defensive programming as a design discipline

Now extract the principles.

I'd put perhaps eight on screen:
1. Assume failure

Every external operation has a failure mode.
2. Bound everything

RAM, connections, queues, retries, time, filesystem usage.
3. Make degradation explicit

Decide what gets sacrificed first.
4. Preserve evidence

A failure without evidence is almost indistinguishable from randomness.
5. Separate active from staging state

Never destroy your last known-good state unnecessarily.
6. Make recovery idempotent

Re-running recovery should be safe.
7. Test the transitions, not just the happy path

Especially:

    "What happens if power disappears right here?"

8. Treat dependencies as part of your system

Sming, lwIP, filesystem, bootloader, compiler, browser, Wi-Fi AP.

You don't get to say:

    "That's the library's problem."

If your device depends on it, it's your failure mode.
The ending

I'd come back to the 03:17 crash.

Imagine two developers.
Developer A

    "The device crashed. I can't reproduce it."

Developer B

    "The device crashed at 03:17:42. It had 6.9 KB free heap, was processing an mDNS response, had been online for 11 days, the previous five log entries were these, the active firmware was this build, the stack points here, and the device recovered on the next boot."

Then:

    "Neither developer got lucky."

    "One of them designed for failure."

And finish with:

    Your microcontroller is going to fail.

    The only question is whether it will fail silently.

That feels much more like a CCC talk than a conventional embedded conference presentation.
And I'd keep the person/project slightly anonymous at first

I actually wouldn't open with:

    "Today I'm going to present the excellent engineering of project X."

I'd tell the story first.

Let the audience discover that the seemingly ordinary ESP8266 lighting project contains:

    its own telemetry infrastructure;

    custom compression;

    crash diagnostics;

    transactional OTA;

    distributed discovery;

    resource admission control;

    swarm testing;

    host memory analysis;

    framework bug investigations.

Then near the end:

    "And all of this came out of one person's attempt to make some lights work reliably."

That makes the point much more effectively.

The underlying message isn't "look how clever this developer is."

It's:

    "This is what happens when you refuse to accept 'it crashed, I don't know why' as an engineering answer."

That, I think, is a very good CCC talk.




===============================



alternative Version

I'd make the opening revolve around:

    "I don't know" is a perfectly good answer.

And then immediately make the distinction:

    "I don't know" is a good starting point.
    "I don't know, and I have no way of finding out" is an engineering failure.

That's essentially the entire talk in two sentences.
The opening could be very simple

Dark screen.

    I don't know.

Pause.

Then:

    Richard Feynman was fond of the idea that admitting "I don't know" is not a failure. It's an honest description of the state of your knowledge.

Then:

    That's particularly important when debugging embedded systems.

Because when an ESP8266 crashes at 03:17, the honest answer is often:

    "I don't know."

And that's fine.

Now what?
Then introduce the central distinction

I'd put this on a slide:

              I DON'T KNOW
                   │
          ┌────────┴────────┐
          │                 │
      curiosity          resignation
          │                 │
     "How can I find      "It happens
       out?"               sometimes."
          │
          ▼
       ENGINEERING

And say:

    The difference between those two reactions is, to me, a large part of what engineering is.

That gives you permission throughout the talk to show uncertainty.

The developer didn't magically know why the TCP behaviour occurred.

They didn't know why the OTA sometimes failed.

They didn't know why the heap disappeared.

They didn't know what mDNS would deliver first.

They investigated.

And crucially, they built systems that made the next "I don't know" easier to resolve.
Then the project becomes the case study

You can introduce it almost apologetically:

    "This story starts with a rather ordinary hobby project: some ESP8266s controlling lights."

Audience expects IoT.

Then:

    "And then things started breaking."

First question:

    Why did it crash?

Answer:

    "I don't know."

So:

instrument it.
Next:

    Why did the logs disappear?

    "I don't know."

So:

buffer them. Compress them. Send them somewhere else.
Next:

    Why are the UDP packets disappearing?

    "I don't know."

So:

measure. Capture packets. Inspect lwIP. Discover resource exhaustion. Pace transmission.
Next:

    Why did the update brick the device?

    "I don't know."

So:

persist state. Stage updates. Verify them. Recover after reboot.
Next:

    Why does this mDNS discovery occasionally produce incomplete devices?

    "I don't know."

So:

model asynchronous arrival of information instead of assuming packet order.
Next:

    Why does Sming behave like this?

    "I don't know."

So:

go down the stack.

And now the audience has seen the central idea repeatedly without you ever having to preach it.
The beautiful part is that "I don't know" appears at multiple levels

You can make this a recurring visual motif.
Application developer

    I don't know why the device crashed.

↓
Embedded engineer

    I don't know what the heap looked like.

↓
Network engineer

    I don't know whether the packet left the device.

↓
Framework developer

    I don't know what Sming is doing here.

↓
OS/network-stack developer

    I don't know what lwIP did with the buffer.

↓
Compiler/runtime level

    I don't know what code the compiler generated.

↓
Hardware

    I don't know what the silicon actually did.

And the answer at every level is:

    Measure.

That gives you a fantastic transition into instrumentation.
Which leads to what I think should be the central line

Not:

    "Defensive programming means checking errors."

But:

    Defensive engineering is designing systems so that "I don't know" doesn't remain the final answer.

That's a considerably more interesting proposition.

And it explains all of the apparently unrelated technology:
Problem	Response
Crash	Preserve evidence
RAM pressure	Measure and shed load
Lost logs	Buffer and compress
Network loss	Retry / pace / retain
Filesystem interruption	Stage and verify
OTA failure	Persist state / recover
mDNS uncertainty	Model partial information
Distributed state	Make election deterministic
Framework bug	Reproduce below the abstraction
Regression	Put it in CI

They're all variations of the same response to:

    "I don't know."

I'd actually be careful with the Feynman attribution

If you use the exact phrase publicly, I'd verify the precise source/wording rather than presenting a paraphrase as a quotation. The underlying Feynman idea is excellent, but you don't need to turn the opening into a quotation-history discussion.

You could simply say:

    "There's a wonderful scientific habit associated with Richard Feynman: being willing to say I don't know."

Then make it your own thesis.

Because the really interesting statement isn't Feynman's.

It's yours:

    "I don't know" is perfectly acceptable.
    "I don't know, and I designed the system so that I'll never be able to find out" isn't.

That feels like the intellectual spine of the whole talk.

And suddenly "defensive microcontroller programming" becomes almost a secondary topic.

The real talk is about epistemic humility turned into engineering practice.

That's considerably more interesting.
