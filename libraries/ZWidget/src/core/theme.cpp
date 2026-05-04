
#include "core/theme.h"
#include "core/widget.h"
#include "core/canvas.h"

void WidgetStyle::SetBool(const std::string& state, const std::string& propertyName, bool value)
{
	StyleProperties[state][propertyName] = value;
}

void WidgetStyle::SetInt(const std::string& state, const std::string& propertyName, int value)
{
	StyleProperties[state][propertyName] = value;
}

void WidgetStyle::SetDouble(const std::string& state, const std::string& propertyName, double value)
{
	StyleProperties[state][propertyName] = value;
}

void WidgetStyle::SetString(const std::string& state, const std::string& propertyName, const std::string& value)
{
	StyleProperties[state][propertyName] = value;
}

void WidgetStyle::SetColor(const std::string& state, const std::string& propertyName, const Colorf& value)
{
	StyleProperties[state][propertyName] = value;
}

const WidgetStyle::PropertyVariant* WidgetStyle::FindProperty(const std::string& state, const std::string& propertyName) const
{
	const WidgetStyle* style = this;
	do
	{
		// Look for property in the specific state
		auto stateIt = style->StyleProperties.find(state);
		if (stateIt != style->StyleProperties.end())
		{
			auto it = stateIt->second.find(propertyName);
			if (it != stateIt->second.end())
				return &it->second;
		}

		// Fall back to the widget main style
		if (state != std::string())
		{
			stateIt = style->StyleProperties.find(std::string());
			if (stateIt != style->StyleProperties.end())
			{
				auto it = stateIt->second.find(propertyName);
				if (it != stateIt->second.end())
					return &it->second;
			}
		}

		style = style->ParentStyle;
	} while (style);
	return nullptr;
}

bool WidgetStyle::GetBool(const std::string& state, const std::string& propertyName) const
{
	const PropertyVariant* prop = FindProperty(state, propertyName);
	return prop ? std::get<bool>(*prop) : false;
}

int WidgetStyle::GetInt(const std::string& state, const std::string& propertyName) const
{
	const PropertyVariant* prop = FindProperty(state, propertyName);
	return prop ? std::get<int>(*prop) : 0;
}

double WidgetStyle::GetDouble(const std::string& state, const std::string& propertyName) const
{
	const PropertyVariant* prop = FindProperty(state, propertyName);
	return prop ? std::get<double>(*prop) : 0.0;
}

std::string WidgetStyle::GetString(const std::string& state, const std::string& propertyName) const
{
	const PropertyVariant* prop = FindProperty(state, propertyName);
	return prop ? std::get<std::string>(*prop) : std::string();
}

Colorf WidgetStyle::GetColor(const std::string& state, const std::string& propertyName) const
{
	const PropertyVariant* prop = FindProperty(state, propertyName);
	return prop ? std::get<Colorf>(*prop) : Colorf::transparent();
}

/////////////////////////////////////////////////////////////////////////////

void BasicWidgetStyle::Paint(Widget* widget, Canvas* canvas, Size size)
{
	Colorf bgcolor = widget->GetStyleColor("background-color");
	if (bgcolor.a > 0.0f)
		canvas->fillRect(Rect::xywh(0.0, 0.0, size.width, size.height), bgcolor);

	Colorf borderleft = widget->GetStyleColor("border-left-color");
	Colorf bordertop = widget->GetStyleColor("border-top-color");
	Colorf borderright = widget->GetStyleColor("border-right-color");
	Colorf borderbottom = widget->GetStyleColor("border-bottom-color");

	double borderwidth = widget->GridFitSize(1.0);

	if (bordertop.a > 0.0f)
		canvas->fillRect(Rect::xywh(0.0, 0.0, size.width, borderwidth), bordertop);
	if (borderbottom.a > 0.0f)
		canvas->fillRect(Rect::xywh(0.0, size.height - borderwidth, size.width, borderwidth), borderbottom);
	if (borderleft.a > 0.0f)
		canvas->fillRect(Rect::xywh(0.0, 0.0, borderwidth, size.height), borderleft);
	if (borderright.a > 0.0f)
		canvas->fillRect(Rect::xywh(size.width - borderwidth, 0.0, borderwidth, size.height), borderright);
}

/////////////////////////////////////////////////////////////////////////////

static std::unique_ptr<WidgetTheme> CurrentTheme;

WidgetStyle* WidgetTheme::RegisterStyle(std::unique_ptr<WidgetStyle> widgetStyle, const std::string& widgetClass)
{
	auto& style = Styles[widgetClass];
	style = std::move(widgetStyle);
	return style.get();
}

WidgetStyle* WidgetTheme::GetStyle(const std::string& widgetClass)
{
	auto it = Styles.find(widgetClass);
	return it != Styles.end() ? it->second.get() : nullptr;
}

void WidgetTheme::SetTheme(std::unique_ptr<WidgetTheme> theme)
{
	CurrentTheme = std::move(theme);
}

WidgetTheme* WidgetTheme::GetTheme()
{
	return CurrentTheme.get();
}

WidgetTheme::WidgetTheme(const struct SimpleTheme &theme)
{

	auto bgMain   = theme.bgMain;   // background
	auto fgMain   = theme.fgMain;   //
	auto bgLight  = theme.bgLight;  // headers / inputs
	auto fgLight  = theme.fgLight;  //
	auto bgAction = theme.bgAction; // interactive elements
	auto fgAction = theme.fgAction; //
	auto bgHover  = theme.bgHover;  // hover / highlight
	auto fgHover  = theme.fgHover;  //
	auto bgActive = theme.bgActive; // click
	auto fgActive = theme.fgActive; //
	auto border   = theme.border;   // around elements
	auto divider  = theme.divider;  // between elements

	auto none   = Colorf::transparent();

	auto widget = RegisterStyle(std::make_unique<BasicWidgetStyle>(), "widget");
	/*auto textlabel =*/ RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "textlabel");
	auto pushbutton = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "pushbutton");
	auto lineedit = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "lineedit");
	auto textedit = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "textedit");
	auto listview = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "listview");
	auto dropdown = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "dropdown");
	auto scrollbar = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "scrollbar");
	auto tabbar = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "tabbar");
	auto tabbar_tab = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "tabbar-tab");
	auto tabbar_spacer = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "tabbar-spacer");
	auto tabwidget_stack = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "tabwidget-stack");
	auto checkbox_label = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "checkbox-label");
	auto menubar = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "menubar");
	auto menubaritem = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "menubaritem");
	auto menu = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "menu");
	auto menuitem = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "menuitem");
	auto toolbar = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "toolbar");
	auto toolbarbutton = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "toolbarbutton");
	auto statusbar = RegisterStyle(std::make_unique<BasicWidgetStyle>(widget), "statusbar");

	widget->SetString("font-family", "NotoSans");
	widget->SetColor("color", fgMain);
	widget->SetColor("window-background", bgMain);
	widget->SetColor("window-border", bgMain);
	widget->SetColor("window-caption-color", bgLight);
	widget->SetColor("window-caption-text-color", fgLight);

	pushbutton->SetDouble("noncontent-left", 10.0);
	pushbutton->SetDouble("noncontent-top", 5.0);
	pushbutton->SetDouble("noncontent-right", 10.0);
	pushbutton->SetDouble("noncontent-bottom", 5.0);
	pushbutton->SetColor("color", fgAction);
	pushbutton->SetColor("background-color", bgAction);
	pushbutton->SetColor("border-left-color", border);
	pushbutton->SetColor("border-top-color", border);
	pushbutton->SetColor("border-right-color", border);
	pushbutton->SetColor("border-bottom-color", border);
	pushbutton->SetColor("hover", "color", fgHover);
	pushbutton->SetColor("hover", "background-color", bgHover);
	pushbutton->SetColor("down", "color", fgActive);
	pushbutton->SetColor("down", "background-color", bgActive);

	lineedit->SetDouble("noncontent-left", 5.0);
	lineedit->SetDouble("noncontent-top", 3.0);
	lineedit->SetDouble("noncontent-right", 5.0);
	lineedit->SetDouble("noncontent-bottom", 3.0);
	lineedit->SetColor("color", fgLight);
	lineedit->SetColor("background-color", bgLight);
	lineedit->SetColor("border-left-color", border);
	lineedit->SetColor("border-top-color", border);
	lineedit->SetColor("border-right-color", border);
	lineedit->SetColor("border-bottom-color", border);
	lineedit->SetColor("selection-color", bgHover);
	lineedit->SetColor("no-focus-selection-color", bgHover);

	textedit->SetDouble("noncontent-left", 8.0);
	textedit->SetDouble("noncontent-top", 8.0);
	textedit->SetDouble("noncontent-right", 8.0);
	textedit->SetDouble("noncontent-bottom", 8.0);
	textedit->SetColor("color", fgLight);
	textedit->SetColor("background-color", bgLight);
	textedit->SetColor("border-left-color", border);
	textedit->SetColor("border-top-color", border);
	textedit->SetColor("border-right-color", border);
	textedit->SetColor("border-bottom-color", border);
	textedit->SetColor("selection-color", bgHover);

	listview->SetDouble("noncontent-left", 10.0);
	listview->SetDouble("noncontent-top", 10.0);
	listview->SetDouble("noncontent-right", 3.0);
	listview->SetDouble("noncontent-bottom", 10.0);
	listview->SetColor("color", fgLight);
	listview->SetColor("background-color", bgLight);
	listview->SetColor("border-left-color", border);
	listview->SetColor("border-top-color", border);
	listview->SetColor("border-right-color", border);
	listview->SetColor("border-bottom-color", border);
	listview->SetColor("selection-color", bgHover);

	dropdown->SetDouble("noncontent-left", 5.0);
	dropdown->SetDouble("noncontent-top", 5.0);
	dropdown->SetDouble("noncontent-right", 5.0);
	dropdown->SetDouble("noncontent-bottom", 5.0);
	dropdown->SetColor("color", fgLight);
	dropdown->SetColor("background-color", bgLight);
	dropdown->SetColor("border-left-color", border);
	dropdown->SetColor("border-top-color", border);
	dropdown->SetColor("border-right-color", border);
	dropdown->SetColor("border-bottom-color", border);
	dropdown->SetColor("arrow-color", border);

	scrollbar->SetColor("track-color", divider);
	scrollbar->SetColor("thumb-color", border);

	tabbar->SetDouble("spacer-left", 20.0);
	tabbar->SetDouble("spacer-right", 20.0);
	tabbar->SetColor("background-color", bgLight);

	tabbar_tab->SetDouble("noncontent-left", 15.0);
	tabbar_tab->SetDouble("noncontent-right", 15.0);
	tabbar_tab->SetDouble("noncontent-top", 1.0);
	tabbar_tab->SetDouble("noncontent-bottom", 1.0);
	tabbar_tab->SetColor("color", fgMain);
	tabbar_tab->SetColor("background-color", bgMain);
	tabbar_tab->SetColor("border-left-color", divider);
	tabbar_tab->SetColor("border-top-color", divider);
	tabbar_tab->SetColor("border-right-color", divider);
	tabbar_tab->SetColor("border-bottom-color", border);
	tabbar_tab->SetColor("hover", "color", fgAction);
	tabbar_tab->SetColor("hover", "background-color", bgAction);
	tabbar_tab->SetColor("active", "background-color", bgMain);
	tabbar_tab->SetColor("active", "border-left-color", border);
	tabbar_tab->SetColor("active", "border-top-color", border);
	tabbar_tab->SetColor("active", "border-right-color", border);
	tabbar_tab->SetColor("active", "border-bottom-color", none);

	tabbar_spacer->SetDouble("noncontent-bottom", 1.0);
	tabbar_spacer->SetColor("border-bottom-color", border);

	tabwidget_stack->SetDouble("noncontent-left", 20.0);
	tabwidget_stack->SetDouble("noncontent-top", 5.0);
	tabwidget_stack->SetDouble("noncontent-right", 20.0);
	tabwidget_stack->SetDouble("noncontent-bottom", 5.0);

	checkbox_label->SetColor("checked-outer-border-color", border);
	checkbox_label->SetColor("checked-inner-border-color", bgMain);
	checkbox_label->SetColor("checked-color", fgMain);
	checkbox_label->SetColor("unchecked-outer-border-color", border);
	checkbox_label->SetColor("unchecked-inner-border-color", bgMain);

	menubar->SetColor("background-color", bgLight);
	toolbar->SetColor("background-color", bgLight);
	statusbar->SetColor("background-color", bgLight);

	toolbarbutton->SetColor("hover", "color", fgHover);
	toolbarbutton->SetColor("hover", "background-color", bgHover);
	toolbarbutton->SetColor("down", "color", fgActive);
	toolbarbutton->SetColor("down", "background-color", bgActive);

	menubaritem->SetColor("color", fgMain);
	menubaritem->SetColor("hover", "color", fgHover);
	menubaritem->SetColor("hover", "background-color", bgHover);
	menubaritem->SetColor("down", "color", fgActive);
	menubaritem->SetColor("down", "background-color", bgActive);

	menu->SetDouble("noncontent-left", 5.0);
	menu->SetDouble("noncontent-top", 5.0);
	menu->SetDouble("noncontent-right", 5.0);
	menu->SetDouble("noncontent-bottom", 5.0);
	menu->SetColor("color", fgMain);
	menu->SetColor("background-color", bgMain);
	menu->SetColor("border-left-color", border);
	menu->SetColor("border-top-color", border);
	menu->SetColor("border-right-color", border);
	menu->SetColor("border-bottom-color", border);

	menuitem->SetColor("hover", "color", fgHover);
	menuitem->SetColor("hover", "background-color", bgHover);
	menuitem->SetColor("down", "color", fgActive);
	menuitem->SetColor("down", "background-color", bgActive);
}

/////////////////////////////////////////////////////////////////////////////

WidgetTheme::SimpleTheme DarkWidgetTheme::GetSimpleTheme()
{
	return {
		Colorf(0x2A, 0x2A, 0x2A), // background
		Colorf(0xE2, 0xDF, 0xDB), //
		Colorf(0x21, 0x21, 0x21), // headers / inputs
		Colorf(0xE2, 0xDF, 0xDB), //
		Colorf(0x44, 0x44, 0x44), // interactive elements
		Colorf(0xFF, 0xFF, 0xFF), //
		Colorf(0xC8, 0x3C, 0x00), // hover / highlight
		Colorf(0xFF, 0xFF, 0xFF), //
		Colorf(0xBB, 0xBB, 0xBB), // click
		Colorf(0x00, 0x00, 0x00), //
		Colorf(0x64, 0x64, 0x64), // around elements
		Colorf(0x55, 0x55, 0x55)  // between elements
	};
}

DarkWidgetTheme::DarkWidgetTheme(): WidgetTheme(GetSimpleTheme()) {};

/////////////////////////////////////////////////////////////////////////////

WidgetTheme::SimpleTheme LightWidgetTheme::GetSimpleTheme()
{
	return {
		Colorf(0xF0, 0xF0, 0xF0), // background
		Colorf(0x19, 0x19, 0x19), //
		Colorf(0xFA, 0xFA, 0xFA), // headers / inputs
		Colorf(0x19, 0x19, 0x19), //
		Colorf(0xC8, 0xC8, 0xC8), // interactive elements
		Colorf(0x00, 0x00, 0x00), //
		Colorf(0xD2, 0xD2, 0xFF), // hover / highlight
		Colorf(0x00, 0x00, 0x00), //
		Colorf(0xC7, 0xB4, 0xFF), // click
		Colorf(0x00, 0x00, 0x00), //
		Colorf(0xA0, 0xA0, 0xA0), // around elements
		Colorf(0xB9, 0xB9, 0xB9)  // between elements
	};
}

LightWidgetTheme::LightWidgetTheme(): WidgetTheme(GetSimpleTheme()) {};

/////////////////////////////////////////////////////////////////////////////

#include <fstream>
#include <sstream>

class POSIXNativeThemeImpl
{
public:
	static WidgetTheme::SimpleTheme DetectColors()
	{
		WidgetTheme::SimpleTheme theme = DarkWidgetTheme::GetSimpleTheme();

#if defined(UNIX) && !defined(__APPLE__)
		bool detected = false;
		const char* home = std::getenv("HOME");

		// 1. Try KDE detection
		if (home && !detected)
		{
			std::string kdepath = std::string(home) + "/.config/kdeglobals";
			std::ifstream f(kdepath);
			if (f.is_open())
			{
				std::string line;
				bool inColorsWindow = false;
				while (std::getline(f, line))
				{
					if (line == "[Colors:Window]") inColorsWindow = true;
					else if (line.length() > 0 && line[0] == '[') inColorsWindow = false;

					if (inColorsWindow)
					{
						if (line.compare(0, 17, "BackgroundNormal=") == 0) {
							theme.bgMain = ParseKDEColor(line.substr(17));
							detected = true;
						}
						else if (line.compare(0, 17, "ForegroundNormal=") == 0)
							theme.fgMain = ParseKDEColor(line.substr(17));
					}
				}
			}
		}

		// 2. Try GTK settings.ini detection (XFCE, MATE, GNOME fallback)
		if (home && !detected)
		{
			std::string gtkpath = std::string(home) + "/.config/gtk-3.0/settings.ini";
			std::ifstream f(gtkpath);
			if (f.is_open())
			{
				std::string line;
				while (std::getline(f, line))
				{
					if (line.find("gtk-application-prefer-dark-theme=1") != std::string::npos ||
					    line.find("gtk-application-prefer-dark-theme=true") != std::string::npos)
					{
						theme = DarkWidgetTheme::GetSimpleTheme();
						detected = true;
						break;
					}
				}
			}
		}

		// 3. Try Xresources (Sovereign/Tiling WM setup)
		if (home && !detected)
		{
			std::string xpath = std::string(home) + "/.Xresources";
			std::ifstream f(xpath);
			if (!f.is_open()) {
				xpath = std::string(home) + "/.Xdefaults";
				f.open(xpath);
			}

			if (f.is_open())
			{
				std::string line;
				while (std::getline(f, line))
				{
					if (line.find("background:") != std::string::npos || line.find("*.background:") != std::string::npos)
						theme.bgMain = ParseXColor(line);
					else if (line.find("foreground:") != std::string::npos || line.find("*.foreground:") != std::string::npos)
						theme.fgMain = ParseXColor(line);
				}
				detected = true;
			}
		}
#endif
		return theme;
	}

private:
#if defined(UNIX) && !defined(__APPLE__)
	static Colorf ParseKDEColor(const std::string& s)
	{
		int r, g, b;
		if (sscanf(s.c_str(), "%d,%d,%d", &r, &g, &b) == 3)
			return Colorf(r, g, b);
		return Colorf::white();
	}

	static Colorf ParseXColor(const std::string& line)
	{
		size_t pos = line.find("#");
		if (pos != std::string::npos)
		{
			std::string hex = line.substr(pos + 1);
			if (hex.length() >= 6)
			{
				unsigned int r, g, b;
				if (sscanf(hex.c_str(), "%02x%02x%02x", &r, &g, &b) == 3)
					return Colorf((int)r, (int)g, (int)b);
			}
		}
		return Colorf::white();
	}
#endif
};

POSIXNativeTheme::POSIXNativeTheme() : WidgetTheme(POSIXNativeThemeImpl::DetectColors())
{
}
