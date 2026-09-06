class_name HudStyle
extends RefCounted

## Fonts, colors and style boxes of the overlay, in one place.
##
## Two families: Inter for the labels and the titles, JetBrains Mono for
## every number and identifier, so columns of values line up. The fonts are
## read straight from assets/fonts/ at start (no import step: the folder is
## ignored by the editor); when one cannot be read the engine's fallback
## font takes its place and the overlay stays readable.

const FONT_DIR := "res://assets/fonts/"

const TEXT := Color(0.92, 0.93, 0.95)
const MUTED := Color(0.60, 0.64, 0.70)
const ACCENT := Color(0.30, 0.80, 0.98)
const GOOD := Color(0.35, 0.85, 0.50)
const WARN := Color(0.98, 0.72, 0.20)
const BAD := Color(0.95, 0.30, 0.30)
const PANEL := Color(0.05, 0.06, 0.08, 0.78)
const PANEL_BORDER := Color(1.0, 1.0, 1.0, 0.08)
const TRACK := Color(1.0, 1.0, 1.0, 0.08)
const ROW_HOVER := Color(1.0, 1.0, 1.0, 0.06)
const ROW_SELECTED := Color(0.30, 0.80, 0.98, 0.18)

const TITLE_SIZE := 11
const LABEL_SIZE := 13
const MONO_SIZE := 13
const HINT_SIZE := 12

var label_font: Font
var title_font: Font
var mono_font: Font
var mono_bold_font: Font


func _init() -> void:
	label_font = _load("Inter-Medium.ttf")
	title_font = _load("Inter-SemiBold.ttf")
	mono_font = _load("JetBrainsMono-Regular.ttf")
	mono_bold_font = _load("JetBrainsMono-Bold.ttf")


## Read one font file, or fall back to the engine font.
static func _load(file_name: String) -> Font:
	var font := FontFile.new()
	var error := font.load_dynamic_font(FONT_DIR + file_name)
	if error != OK:
		push_warning("hud: font %s not readable (%d), using the fallback" % [file_name, error])
		return ThemeDB.fallback_font
	font.antialiasing = TextServer.FONT_ANTIALIASING_GRAY
	font.hinting = TextServer.HINTING_LIGHT
	font.subpixel_positioning = TextServer.SUBPIXEL_POSITIONING_AUTO
	return font


## Translucent dark card with a hairline border.
func panel_box(radius_px: int = 8) -> StyleBoxFlat:
	var box := StyleBoxFlat.new()
	box.bg_color = PANEL
	box.border_color = PANEL_BORDER
	box.set_border_width_all(1)
	box.set_corner_radius_all(radius_px)
	box.content_margin_left = 12.0
	box.content_margin_right = 12.0
	box.content_margin_top = 10.0
	box.content_margin_bottom = 10.0
	return box


## Rounded pill tinted with a color, for the phase.
func pill_box(color: Color) -> StyleBoxFlat:
	var box := StyleBoxFlat.new()
	box.bg_color = Color(color, 0.22)
	box.border_color = Color(color, 0.7)
	box.set_border_width_all(1)
	box.set_corner_radius_all(999)
	box.content_margin_left = 12.0
	box.content_margin_right = 12.0
	box.content_margin_top = 3.0
	box.content_margin_bottom = 3.0
	return box


## Flat rounded fill, for list rows.
func flat_box(color: Color) -> StyleBoxFlat:
	var box := StyleBoxFlat.new()
	box.bg_color = color
	box.set_corner_radius_all(6)
	box.content_margin_left = 8.0
	box.content_margin_right = 8.0
	box.content_margin_top = 4.0
	box.content_margin_bottom = 4.0
	return box


## A label in the given font.
func label(text: String, font: Font, size: int, color: Color) -> Label:
	var node := Label.new()
	node.text = text
	node.add_theme_font_override("font", font)
	node.add_theme_font_size_override("font_size", size)
	node.add_theme_color_override("font_color", color)
	node.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return node


## Small uppercase section title.
func title(text: String) -> Label:
	return label(text.to_upper(), title_font, TITLE_SIZE, MUTED)


## Monospace value.
func mono(text: String, color: Color = TEXT) -> Label:
	return label(text, mono_font, MONO_SIZE, color)


## Regular text.
func text(text: String, color: Color = TEXT) -> Label:
	return label(text, label_font, LABEL_SIZE, color)
