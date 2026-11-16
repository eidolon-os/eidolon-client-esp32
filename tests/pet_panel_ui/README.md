# StackChan control panel and pet local-life checks

Run `bash tests/run_pet_panel_ui_tests.sh` from the ESP32 repository. It builds
`CompanionFaceView` as StackChan builds it (`CONFIG_EIDOLON_PET_FACE`, the
pull-down control panel, the desk-pet skin and runtime) on the resolved
`managed_components` LVGL. The test replaces only these:

- the pet's body (`Board`: senses, power, LEDs, head moves, which are counted)
- the audio levels
- chirp playback (counted)
- the FreeRTOS sense queue
- the display lock

No device, service or network is used.

It drives the view the way a person does, with a finger on the screen and a tap
on the head, and asserts the rules the view owns around a start or end:

- A finger held on a panel button past the panel's 5 s idle close keeps the
  panel open, and the release still acts. With no finger on it, the panel still
  closes on its own.
- While an end is drawn ahead of the runtime (`session_request_pending`; the
  microphone may still be open), the pet neither chirps nor moves its head on
  its own. Once the runtime confirms, the same tap is answered.

The build directory defaults to `$TMPDIR/eidolon-pet-panel-ui-tests`. Set
`EIDOLON_UI_TEST_BUILD` to override it, for example when running from a
worktree beside another checkout.
