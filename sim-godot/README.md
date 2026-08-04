# Godot simulator

Coming later - standalone Godot project, runs on the HOST (not in the
container). RigidBody3D + Jolt, realistic sensor models (the accelerometer
simulates specific force: 0 g in free fall), parameterizable "throw" command,
viewer mode. Speaks only `protocol/` (UDP), never links flight-core.

godot-tools LSP: port 6005 is forwarded by the devcontainer.
