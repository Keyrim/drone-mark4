class_name SimHud
extends CanvasLayer

## On screen overlay of the plant.
##
## Four cards around the view: the plant itself (node id, wire hash, how
## many drones it hosts) top left, the list of drones top right (click one
## to follow it), the followed drone along the bottom (phase, motors,
## altitude, accelerometer, throws, sensor and RC state, link counters) and
## the camera mode with the key hints bottom right. Short toasts announce a
## flight process joining or leaving. Values refresh a few times per second,
## which is enough to read them and cheap enough to stay out of the way of
## the physics; the whole layer is skipped when the plant is headless.
##
## No pilot state here: this project holds none. Kill, arm and throttle live
## on the RC path, between the cockpit and the flight process.

## Delay between two value refreshes [s].
const REFRESH_PERIOD_S := 0.1
## Distance from the window edges [px].
const MARGIN_PX := 16.0
## How long a toast stays, and how long it takes to fade at the end [s].
const TOAST_LIFETIME_S := 3.0
const TOAST_FADE_S := 0.6
## Space between the cells of the status bar [px].
const BAR_SEPARATION_PX := 22

## Manager whose drones the overlay shows.
@export var drones_path: NodePath
## Camera whose mode the overlay shows.
@export var camera_path: NodePath

var _drones: DroneManager = null
var _camera: CameraRig = null
var _style: HudStyle = null
var _elapsed_s: float = 0.0

var _plant_node: Label
var _plant_wire: Label
var _plant_count: Label
var _list_title: Label
var _rows: VBoxContainer
## Flight process node id -> its row.
var _row_buttons: Dictionary = {}
var _empty: Control
var _status_bar: Control
var _phase_pill: PanelContainer
var _phase_label: Label
## Phase -> pill style box, built on first use.
var _pill_boxes: Dictionary = {}
var _drone_id: Label
var _sim_time: Label
var _motors: MotorBars
var _altitude: Label
var _accel: Label
var _throws: Label
var _imu: Label
var _baro: Label
var _rc: Label
var _link: Label
var _lockstep: Label
var _camera_mode: Label
var _toasts: VBoxContainer
## Toast -> age [s].
var _toast_ages: Dictionary = {}


func _ready() -> void:
	if DisplayServer.get_name() == "headless":
		set_process(false)
		return
	_drones = get_node_or_null(drones_path) as DroneManager
	_camera = get_node_or_null(camera_path) as CameraRig
	if _drones == null:
		push_warning("overlay: no drone manager to watch")
	_style = HudStyle.new()
	_build()
	if _drones != null:
		_drones.drone_added.connect(_on_drone_added)
		_drones.drone_removed.connect(_on_drone_removed)
		_drones.followed_changed.connect(_on_followed_changed)
	if _camera != null:
		_camera.mode_changed.connect(_on_camera_mode)
		_on_camera_mode(_camera.mode)
	_refresh()


func _process(delta: float) -> void:
	_age_toasts(delta)
	_elapsed_s += delta
	if _elapsed_s < REFRESH_PERIOD_S:
		return
	_elapsed_s = 0.0
	_refresh()


func _build() -> void:
	var root := Control.new()
	root.name = "Root"
	root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	root.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(root)

	# Plant card, top left.
	var plant := _panel()
	plant.set_anchors_preset(Control.PRESET_TOP_LEFT)
	plant.position = Vector2(MARGIN_PX, MARGIN_PX)
	root.add_child(plant)
	var plant_box := _column()
	plant.add_child(plant_box)
	plant_box.add_child(_style.title("plant"))
	_plant_node = _style.mono("node --------")
	_plant_wire = _style.mono("wire --------", HudStyle.MUTED)
	_plant_count = _style.text("no drone")
	plant_box.add_child(_plant_node)
	plant_box.add_child(_plant_wire)
	plant_box.add_child(_plant_count)

	# Drone list, top right.
	var list := _panel()
	list.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	list.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	list.offset_right = -MARGIN_PX
	list.offset_top = MARGIN_PX
	root.add_child(list)
	var list_box := _column()
	list.add_child(list_box)
	_list_title = _style.title("drones")
	list_box.add_child(_list_title)
	_rows = VBoxContainer.new()
	_rows.add_theme_constant_override("separation", 2)
	list_box.add_child(_rows)

	# Empty world, center.
	_empty = CenterContainer.new()
	_empty.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_empty.mouse_filter = Control.MOUSE_FILTER_IGNORE
	root.add_child(_empty)
	var empty_box := _column()
	empty_box.alignment = BoxContainer.ALIGNMENT_CENTER
	_empty.add_child(empty_box)
	var empty_title := _style.text("no flight process yet", HudStyle.MUTED)
	empty_title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	var empty_hint := _style.mono("start a drone_sim: its drone appears here", HudStyle.MUTED)
	empty_hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	empty_box.add_child(empty_title)
	empty_box.add_child(empty_hint)

	# Followed drone, bottom center.
	var bar := _panel()
	bar.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	bar.grow_horizontal = Control.GROW_DIRECTION_BOTH
	bar.grow_vertical = Control.GROW_DIRECTION_BEGIN
	bar.offset_bottom = -MARGIN_PX
	root.add_child(bar)
	_status_bar = bar
	var cells := HBoxContainer.new()
	cells.add_theme_constant_override("separation", BAR_SEPARATION_PX)
	cells.alignment = BoxContainer.ALIGNMENT_CENTER
	bar.add_child(cells)

	var pill_cell := _column()
	pill_cell.alignment = BoxContainer.ALIGNMENT_CENTER
	cells.add_child(pill_cell)
	_phase_pill = PanelContainer.new()
	_phase_label = _style.label("no status", _style.title_font, HudStyle.LABEL_SIZE, HudStyle.TEXT)
	_phase_pill.add_child(_phase_label)
	pill_cell.add_child(_phase_pill)

	_drone_id = _style.mono("--------")
	cells.add_child(_cell("drone", _drone_id))
	_sim_time = _style.mono("0.000 s")
	cells.add_child(_cell("sim time", _sim_time))
	_motors = MotorBars.new()
	cells.add_child(_cell("motors", _motors))
	_altitude = _style.mono("0.00 m")
	cells.add_child(_cell("altitude", _altitude))
	_accel = _style.mono("0.00 g")
	cells.add_child(_cell("accel", _accel))
	_throws = _style.mono("0")
	cells.add_child(_cell("throws", _throws))
	var sensors := HBoxContainer.new()
	sensors.add_theme_constant_override("separation", 8)
	_imu = _style.mono("imu", HudStyle.MUTED)
	_baro = _style.mono("baro", HudStyle.MUTED)
	sensors.add_child(_imu)
	sensors.add_child(_baro)
	cells.add_child(_cell("sensors", sensors))
	_rc = _style.mono("--", HudStyle.MUTED)
	cells.add_child(_cell("rc", _rc))
	var link_box := _column()
	_link = _style.mono("tx 0  rx 0  drop 0", HudStyle.MUTED)
	_lockstep = _style.mono("lockstep on", HudStyle.WARN)
	_lockstep.visible = false
	link_box.add_child(_link)
	link_box.add_child(_lockstep)
	cells.add_child(_cell("link", link_box))

	# Camera and keys, bottom right.
	var camera := _panel()
	camera.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	camera.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	camera.grow_vertical = Control.GROW_DIRECTION_BEGIN
	camera.offset_right = -MARGIN_PX
	camera.offset_bottom = -MARGIN_PX
	root.add_child(camera)
	var camera_box := _column()
	camera.add_child(camera_box)
	camera_box.add_child(_style.title("camera"))
	_camera_mode = _style.mono("chase", HudStyle.ACCENT)
	camera_box.add_child(_camera_mode)
	var keys := GridContainer.new()
	keys.columns = 2
	keys.add_theme_constant_override("h_separation", 10)
	keys.add_theme_constant_override("v_separation", 1)
	camera_box.add_child(keys)
	for pair: Array in [
		["C", "camera mode"],
		["wheel", "zoom"],
		["TAB", "next drone"],
		["click", "pick a drone"],
		["ESC", "quit"],
	]:
		keys.add_child(_style.label(pair[0], _style.mono_font, HudStyle.HINT_SIZE, HudStyle.MUTED))
		keys.add_child(_style.label(pair[1], _style.label_font, HudStyle.HINT_SIZE, HudStyle.MUTED))

	# Toasts, top center.
	_toasts = VBoxContainer.new()
	_toasts.set_anchors_preset(Control.PRESET_CENTER_TOP)
	_toasts.grow_horizontal = Control.GROW_DIRECTION_BOTH
	_toasts.grow_vertical = Control.GROW_DIRECTION_END
	_toasts.offset_top = MARGIN_PX
	_toasts.alignment = BoxContainer.ALIGNMENT_BEGIN
	_toasts.add_theme_constant_override("separation", 6)
	_toasts.mouse_filter = Control.MOUSE_FILTER_IGNORE
	root.add_child(_toasts)


func _panel() -> PanelContainer:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", _style.panel_box())
	return panel


func _column() -> VBoxContainer:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	return box


## A titled cell of the status bar.
func _cell(title: String, content: Control) -> VBoxContainer:
	var box := _column()
	box.add_child(_style.title(title))
	box.add_child(content)
	return box


func _refresh() -> void:
	if _drones == null:
		return
	_plant_node.text = "node %08x" % _drones.transport.node_id
	_plant_wire.text = "wire %08x" % WireHash.VALUE
	var count := _drones.drones.size()
	_plant_count.text = "no drone" if count == 0 else ("%d drone%s" % [count, "s" if count > 1 else ""])
	_list_title.text = "DRONES (%d)" % count
	_empty.visible = count == 0
	_refresh_rows()
	var drone := _drones.followed
	_status_bar.visible = drone != null
	if drone == null:
		return
	var link := drone.sim_link
	if link.status_fresh():
		_set_phase(PhaseStyle.label(link.phase), PhaseStyle.color(link.phase))
	else:
		_set_phase("no status", PhaseStyle.UNKNOWN_COLOR)
	_drone_id.text = "%08x" % link.flight_node
	_sim_time.text = "%.3f s" % drone.simulated_time_s()
	_motors.set_levels(link.motor_commands, drone.motor_speeds())
	_altitude.text = "%.2f m" % drone.altitude_m()
	_accel.text = "%.2f g" % drone.sensors.accel_magnitude_g()
	_throws.text = str(link.throw_count)
	_flag(_imu, "imu", link.status_fresh() and link.imu_valid, link.status_fresh())
	_flag(_baro, "baro", link.status_fresh() and link.baro_valid, link.status_fresh())
	if not link.status_fresh():
		_rc.text = "--"
		_rc.add_theme_color_override("font_color", HudStyle.MUTED)
	elif link.rc_link_ok:
		_rc.text = "link ok"
		_rc.add_theme_color_override("font_color", HudStyle.GOOD)
	else:
		_rc.text = "fail-safe"
		_rc.add_theme_color_override("font_color", HudStyle.WARN)
	_link.text = "tx %d  rx %d  drop %d" % [link.packets_sent, link.packets_received, link.packets_dropped]
	_lockstep.visible = link.lockstep
	if link.lockstep:
		_lockstep.text = "lockstep on, %d timeouts" % link.lockstep_timeouts


func _refresh_rows() -> void:
	var followed := _drones.followed
	for node_id: int in _row_buttons:
		var button: Button = _row_buttons[node_id]
		var drone: Drone = _drones.drones.get(node_id)
		if drone == null:
			continue
		var link := drone.sim_link
		var phase := PhaseStyle.label(link.phase) if link.status_fresh() else "no status"
		button.text = "%08x  %-13s %6.2f m" % [node_id, phase, drone.altitude_m()]
		var selected := drone == followed
		button.add_theme_stylebox_override(
			"normal", _style.flat_box(HudStyle.ROW_SELECTED if selected else Color.TRANSPARENT)
		)
		button.add_theme_color_override("font_color", HudStyle.TEXT if selected else HudStyle.MUTED)


func _set_phase(text: String, color: Color) -> void:
	_phase_label.text = text
	_phase_label.add_theme_color_override("font_color", color.lightened(0.3))
	if not _pill_boxes.has(text):
		_pill_boxes[text] = _style.pill_box(color)
	_phase_pill.add_theme_stylebox_override("panel", _pill_boxes[text])


## Color a sensor flag: green when valid, red when not, muted when unknown.
func _flag(label: Label, name: String, valid: bool, known: bool) -> void:
	label.text = name
	var color := HudStyle.MUTED
	if known:
		color = HudStyle.GOOD if valid else HudStyle.BAD
	label.add_theme_color_override("font_color", color)


func _on_drone_added(node_id: int, _drone: Drone) -> void:
	var button := Button.new()
	button.flat = true
	button.alignment = HORIZONTAL_ALIGNMENT_LEFT
	button.focus_mode = Control.FOCUS_NONE
	button.add_theme_font_override("font", _style.mono_font)
	button.add_theme_font_size_override("font_size", HudStyle.MONO_SIZE)
	button.add_theme_color_override("font_hover_color", HudStyle.TEXT)
	button.add_theme_color_override("font_pressed_color", HudStyle.TEXT)
	button.add_theme_color_override("font_hover_pressed_color", HudStyle.TEXT)
	button.add_theme_stylebox_override("normal", _style.flat_box(Color.TRANSPARENT))
	button.add_theme_stylebox_override("hover", _style.flat_box(HudStyle.ROW_HOVER))
	button.add_theme_stylebox_override("pressed", _style.flat_box(HudStyle.ROW_SELECTED))
	button.add_theme_stylebox_override("hover_pressed", _style.flat_box(HudStyle.ROW_SELECTED))
	button.add_theme_stylebox_override("focus", StyleBoxEmpty.new())
	button.pressed.connect(_drones.follow.bind(node_id))
	_rows.add_child(button)
	_row_buttons[node_id] = button
	_toast("flight process %08x joined" % node_id)
	_refresh()


func _on_drone_removed(node_id: int) -> void:
	var button: Button = _row_buttons.get(node_id)
	if button != null:
		_row_buttons.erase(node_id)
		button.queue_free()
	_toast("flight process %08x left" % node_id)
	_refresh()


func _on_followed_changed(_drone: Drone) -> void:
	_refresh()


func _on_camera_mode(mode: CameraRig.Mode) -> void:
	_camera_mode.text = CameraRig.MODE_NAMES[mode]


func _toast(text: String) -> void:
	var panel := _panel()
	panel.add_child(_style.mono(text))
	_toasts.add_child(panel)
	_toast_ages[panel] = 0.0


func _age_toasts(delta: float) -> void:
	for panel: Control in _toast_ages.keys():
		var age: float = _toast_ages[panel] + delta
		if age >= TOAST_LIFETIME_S:
			_toast_ages.erase(panel)
			panel.queue_free()
			continue
		_toast_ages[panel] = age
		panel.modulate.a = clampf((TOAST_LIFETIME_S - age) / TOAST_FADE_S, 0.0, 1.0)
