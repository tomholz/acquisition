#include "searchcombobox.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QLineEdit>

// Set the width of the popup view to the width of the completer options
void SearchComboCompleter::complete(const QRect& rect) {
    if (popup() == nullptr) {
        return;
    };
    const int rows = completionModel()->rowCount();
    const int width = (rows > 0) ? popup()->sizeHintForColumn(0) : 0;
    popup()->setMinimumWidth(width);
    QCompleter::complete(rect);
}

// Return a custom shortened dalay for the combox box hover tooltip
int SearchComboStyle::styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* returnData) const {
    if (hint == QStyle::SH_ToolTip_WakeUpDelay) {
        return TOOLTIP_DELAY_MSEC;
    } else {
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    };
}

SearchComboBox::SearchComboBox(QAbstractItemModel* model, QWidget* parent) :
    QComboBox(parent),
    completer_(model, this)
{
    setEditable(true);
    setModel(model);
    setCompleter(nullptr);
    setInsertPolicy(QComboBox::NoInsert);
    setStyle(new SearchComboStyle(style()));

    view()->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);

    completer_.setCompletionMode(QCompleter::PopupCompletion);
    completer_.setFilterMode(Qt::MatchContains);
    completer_.setCaseSensitivity(Qt::CaseInsensitive);
    completer_.setModelSorting(QCompleter::CaseInsensitivelySortedModel);
    completer_.setWidget(this);

    connect(this, &QComboBox::editTextChanged,
        this, &SearchComboBox::OnTextEdited);

    connect(&edit_timer_, &QTimer::timeout,
        this, &SearchComboBox::OnEditTimeout);

    connect(&completer_, QOverload<const QString&>::of(&QCompleter::activated),
        this, &SearchComboBox::OnCompleterActivated);
}

void SearchComboBox::OnTextEdited() {
    edit_timer_.start(350);
}

void SearchComboBox::OnEditTimeout() {
    edit_timer_.stop();
    const QString& text = lineEdit()->text();
    if (text.isEmpty() == false) {
        completer_.setCompletionPrefix(text);
        completer_.complete();
    };
}

void SearchComboBox::OnCompleterActivated(const QString& text) {
    setCurrentText(text);
    setToolTip(text);
}
