/*
    Copyright 2016-2025 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef MELONDS_TERMSDIALOG_H
#define MELONDS_TERMSDIALOG_H

#include <QDialog>

class TermsDialog : public QDialog
{
Q_OBJECT

public:
    // Bump this ONLY when the wording below changes in substance. Everyone who
    // already agreed is asked again; a plain version upgrade must not re-ask.
    static const int kVersion = 1;

    explicit TermsDialog(QWidget* parent, bool gate = false);

    // First-run gate. Returns false if the user declined -- the app must exit.
    static bool requireAcceptance();

private:
    static QString html();
};

#endif //MELONDS_TERMSDIALOG_H
