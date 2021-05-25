// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef MN_SELECTION_DEFAULTDIALOG_H
#define MN_SELECTION_DEFAULTDIALOG_H

#include <QDialog>

namespace Ui {
    class MnSelectionDialog;
}

class MNModel;
class QTreeWidgetItem;
class MnInfo;

class MnSelectionDialog : public QDialog
{
Q_OBJECT

public:
    explicit MnSelectionDialog(QWidget *parent);
    ~MnSelectionDialog();

    void setModel(MNModel* _mnModel);
    void updateView();
    std::vector<std::string> getSelectedMnAlias();

public Q_SLOTS:
    void viewItemChanged(QTreeWidgetItem*, int);

private:
    Ui::MnSelectionDialog *ui;
    MNModel* mnModel{nullptr};
    int colCheckBoxWidth_treeMode{50};
    // selected MNs alias
    std::vector<std::string> selectedMnList;

    enum {
        COLUMN_CHECKBOX,
        COLUMN_NAME,
        COLUMN_STATUS
    };

    void appendItem(QFlags<Qt::ItemFlag> flgCheckbox,
                    QFlags<Qt::ItemFlag> flgTristate,
                    const QString& mnName,
                    const QString& mnStats);
};

#endif // MN_SELECTION_DEFAULTDIALOG_H
