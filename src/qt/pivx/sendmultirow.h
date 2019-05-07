#ifndef SENDMULTIROW_H
#define SENDMULTIROW_H

#include <QWidget>
#include <QPushButton>
#include <QAction>
#include "walletmodel.h"
#include "amount.h"
#include "qt/pivx/pwidget.h"

class WalletModel;
class SendCoinsRecipient;

namespace Ui {
class SendMultiRow;
class QPushButton;
}

class SendMultiRow : public PWidget
{
    Q_OBJECT

public:
    explicit SendMultiRow(PWidget *parent = nullptr);
    ~SendMultiRow();

    void hideLabels();
    void showLabels();
    void setNumber(int number);
    int getNumber();

    void setModel(WalletModel* model);
    bool validate();
    SendCoinsRecipient getValue();

    /** Return whether the entry is still empty and unedited */
    bool isClear();
    CAmount getAmountValue(QString str);

    void setAddress(const QString& address);
    void setLabel(const QString& label);
    void setFocus();

    QRect getEditLineRect();
    int getEditHeight();
    int getEditWidth();
    int getMenuBtnWidth();

public slots:
    void clear();

signals:
    void removeEntry(SendMultiRow* entry);
    void onContactsClicked(SendMultiRow* entry);
    void onMenuClicked(SendMultiRow* entry);
    //void payAmountChanged();

protected:
    void resizeEvent(QResizeEvent *event) override;
    virtual void enterEvent(QEvent *) override ;
    virtual void leaveEvent(QEvent *) override ;

private slots:
    void amountChanged(const QString&);
    bool addressChanged(const QString&);
    void deleteClicked();
    //void on_payTo_textChanged(const QString& address);
    //void on_addressBookButton_clicked();

private:
    Ui::SendMultiRow *ui;
    QPushButton *iconNumber;
    QAction *btnContact;

    WalletModel *model;
    int displayUnit;
    int number = 0;
    bool isExpanded = false;

    SendCoinsRecipient recipient;

};

#endif // SENDMULTIROW_H
