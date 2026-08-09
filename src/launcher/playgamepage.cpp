
#include "playgamepage.h"
#include "launcherwindow.h"
#include "i_interface.h"
#include "gstrings.h"
#include "version.h"
#include <zwidget/widgets/textlabel/textlabel.h>
#include <zwidget/widgets/listview/listview.h>
#include <zwidget/widgets/lineedit/lineedit.h>
#include <zwidget/widgets/checkboxlabel/checkboxlabel.h>
#include <zwidget/widgets/pushbutton/pushbutton.h>
#include <zwidget/systemdialogs/open_file_dialog.h>

PlayGamePage::PlayGamePage(LauncherWindow* launcher, const FStartupSelectionInfo& info) : Widget(nullptr), Launcher(launcher)
{
	WelcomeLabel = new TextLabel(this);
	VersionLabel = new TextLabel(this);
	VersionLabel->SetTextAlignment(TextLabelAlignment::Right);
	SelectLabel = new TextLabel(this);
	ParametersLabel = new TextLabel(this);
	GamesList = new ListView(this);
	ParametersEdit = new LineEdit(this);
	SaveArgsCheckbox = new CheckboxLabel(this);
	AddFilesButton = new PushButton(this);
	AddFilesButton->OnClick = [=]() { OnAddFilesButtonClicked(); };

	SaveArgsCheckbox->SetChecked(info.bSaveArgs);
	if (!info.DefaultArgs.IsEmpty())
		ParametersEdit->SetText(info.DefaultArgs.GetChars());

	for (const auto& wad : *info.Wads)
	{
		const char* filepart = strrchr(wad.Path.GetChars(), '/');
		if (filepart == nullptr)
			filepart = wad.Path.GetChars();
		else
			++filepart;

		FString work;
		if (*filepart)
			work.Format("%s (%s)", wad.Name.GetChars(), filepart);
		else
			work = wad.Name.GetChars();

		GamesList->AddItem(work.GetChars());
	}

	if (info.DefaultIWAD >= 0 && info.DefaultIWAD < info.Wads->SSize())
	{
		GamesList->SetSelectedItem(info.DefaultIWAD);
		GamesList->ScrollToItem(info.DefaultIWAD);
	}

	GamesList->OnActivated = [=]() { OnGamesListActivated(); };
}

void PlayGamePage::SetValues(FStartupSelectionInfo& info) const
{
	info.DefaultIWAD = GamesList->GetSelectedItem();
	info.DefaultArgs = ParametersEdit->GetText();
	info.bSaveArgs = SaveArgsCheckbox->GetChecked();
}

void PlayGamePage::UpdateLanguage()
{
	SelectLabel->SetText(GStrings.GetString("PICKER_SELECT"));
	ParametersLabel->SetText(GStrings.GetString("PICKER_ADDPARM"));
	FString welcomeText = GStrings.GetString("PICKER_WELCOME");
	welcomeText.Substitute("%s", GAMENAME);
	WelcomeLabel->SetText(welcomeText.GetChars());
	FString versionText = GStrings.GetString("PICKER_VERSION");
	versionText.Substitute("%s", GetVersionString());
	VersionLabel->SetText(versionText.GetChars());
	SaveArgsCheckbox->SetText(GStrings.GetString("PICKER_REMPARM"));
	// No PICKER_* string exists for this yet; add one before translating.
	AddFilesButton->SetText("Add Files...");
}

void PlayGamePage::OnAddFilesButtonClicked()
{
	auto dialog = OpenFileDialog::Create(this);
	if (!dialog)
		return;

	dialog->SetTitle("Select mods to load");
	dialog->SetMultiSelect(true);
	dialog->AddFilter("Doom mods and maps", "*.wad;*.pk3;*.pk7;*.zip;*.7z;*.iwad;*.ipk3;*.deh;*.bex");
	dialog->AddFilter("All files", "*.*");

	if (!dialog->Show())
		return;

	// Append to whatever is already typed rather than replacing it -- the field
	// is shared with hand-written switches and the user may have set some.
	FString params = ParametersEdit->GetText().c_str();
	for (const std::string& filename : dialog->Filenames())
	{
		if (filename.empty())
			continue;

		if (params.Len() > 0)
			params += " ";

		// Quote unconditionally: mod paths routinely contain spaces, and the
		// parameters field is re-parsed as a command line.
		params << "-file \"" << filename.c_str() << "\"";
	}

	ParametersEdit->SetText(params.GetChars());
}

void PlayGamePage::OnGamesListActivated()
{
	Launcher->Start();
}

void PlayGamePage::OnSetFocus()
{
	GamesList->SetFocus();
}

void PlayGamePage::OnGeometryChanged()
{
	double y = 10.0;

	const double halfW = GetWidth() * 0.5;
	WelcomeLabel->SetFrameGeometry(0.0, y, halfW, WelcomeLabel->GetPreferredHeight());
	VersionLabel->SetFrameGeometry(halfW, y, halfW, VersionLabel->GetPreferredHeight());

	y += VersionLabel->GetPreferredHeight() + 10.0;

	SelectLabel->SetFrameGeometry(0.0, y, GetWidth(), SelectLabel->GetPreferredHeight());
	y += SelectLabel->GetPreferredHeight();

	const double listViewTop = y;

	y = GetHeight() - SaveArgsCheckbox->GetPreferredHeight();
	SaveArgsCheckbox->SetFrameGeometry(0.0, y, GetWidth(), SaveArgsCheckbox->GetPreferredHeight());

	const double editHeight = 24.0;
	y -= editHeight + 2.0;
	// Share the row with the browse button rather than adding another line, so
	// the games list keeps its height.
	const double addFilesWidth = 110.0;
	const double addFilesGap = 6.0;
	const double editWidth = std::max(GetWidth() - addFilesWidth - addFilesGap, 0.0);
	ParametersEdit->SetFrameGeometry(0.0, y, editWidth, editHeight);
	AddFilesButton->SetFrameGeometry(editWidth + addFilesGap, y, addFilesWidth, editHeight);

	const double labelHeight = ParametersLabel->GetPreferredHeight();
	y -= labelHeight;
	ParametersLabel->SetFrameGeometry(0.0, y, GetWidth(), labelHeight);

	y -= 20.0;
	GamesList->SetFrameGeometry(0.0, listViewTop, GetWidth(), std::max(y - listViewTop, 0.0));

	Launcher->UpdatePlayButton();
}
