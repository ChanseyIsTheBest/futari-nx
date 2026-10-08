// Scripted controller input for unattended test runs: a synthetic pad (merged into player 1, like the keyboard
// emulation) that presses buttons on a timeline, so a minimized run can get past the title screen and the menus into
// gameplay without anyone at the controls.
//
// futari_input_script (or REX_FUTARI_INPUT_SCRIPT) is a list of steps separated by commas:
//
//   <time>:<buttons>           press for 150 ms             12:start   15.5:a
//   <time>-<time>:<buttons>    hold for the whole range     30-90:a+up
//   <time>-<time>:<buttons>*   rapid presses (50 ms on/off) 30-90:a*
//
// Times are seconds since the game started, or frames presented when written f<N> (f720:start, f900-f1500:a).
// A step for the second controller is written <time>:p2:<buttons> (40:p2:start); with such steps the script adds two
// pads, guest users 0 and 1 (when no real pad is plugged in), instead of merging into player 1.
// Buttons, joined with '+': a b x y start back up down left right lb rb lt rt ls rs. Empty: the driver is not added.

#pragma once

namespace rex {
class Runtime;
}

namespace futari::input_script {

// Adds the scripted pad to the runtime's input system when futari_input_script is set. Call before the game starts.
void Install(rex::Runtime* runtime);

}  // namespace futari::input_script
