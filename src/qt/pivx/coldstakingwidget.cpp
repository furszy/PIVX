// Copyright (c) 2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/coldstakingwidget.h"
#include "qt/pivx/forms/ui_coldstakingwidget.h"
#include "qt/pivx/qtutils.h"
#include "guiutil.h"
#include "qt/pivx/denomgenerationdialog.h"
#include "qt/pivx/txviewholder.h"
#include "walletmodel.h"
#include "optionsmodel.h"
#include "coincontroldialog.h"
#include "coincontrol.h"
#include "zpiv/accumulators.h"

#define DECORATION_SIZE 65
#define NUM_ITEMS 3

ColdStakingWidget::ColdStakingWidget(PIVXGUI* parent) :
    PWidget(parent),
    ui(new Ui::ColdStakingWidget)
{
    ui->setupUi(this);
    this->setStyleSheet(parent->styleSheet());

    /* Containers */
    setCssProperty(ui->left, "container");
    ui->left->setContentsMargins(0,20,0,0);
    setCssProperty(ui->right, "container-right");
    ui->right->setContentsMargins(20,10,20,20);

    /* Light Font */
    QFont fontLight;
    fontLight.setWeight(QFont::Light);

    /* Title */
    ui->labelTitle->setText(tr("Privacy"));
    setCssTitleScreen(ui->labelTitle);
    ui->labelTitle->setFont(fontLight);

    /* Button Group */
    ui->pushLeft->setText(tr("Delegation"));
    setCssProperty(ui->pushLeft, "btn-check-left");
    ui->pushRight->setText(tr("Cold Staker"));
    setCssProperty(ui->pushRight, "btn-check-right");

    /* Subtitle */
    ui->labelSubtitle1->setText(tr("Minting zPIV anonymizes your PIV by removing any\ntransaction history, making transactions untraceable "));
    setCssSubtitleScreen(ui->labelSubtitle1);

    ui->labelSubtitle2->setText(tr("Mint new zPIV or convert back to PIV"));
    setCssSubtitleScreen(ui->labelSubtitle2);
    ui->labelSubtitle2->setContentsMargins(0,2,0,0);
    setCssProperty(ui->labelSubtitleAmount, "text-title");

    ui->lineEditAmount->setPlaceholderText("0.00 PIV ");
    ui->lineEditAmount->setValidator(new QRegExpValidator(QRegExp("[0-9]+")));
    initCssEditLine(ui->lineEditAmount);

    // List
    ui->labelListHistory->setText(tr("No balance delegated"));
    setCssProperty(ui->labelListHistory, "text-title");

    //ui->emptyContainer->setVisible(false);
    setCssProperty(ui->pushImgEmpty, "img-empty-privacy");
    ui->labelEmpty->setText(tr("No transactions yet"));
    setCssProperty(ui->labelEmpty, "text-empty");

    // Buttons
    setCssBtnPrimary(ui->pushButtonSave);

    // Only Convert to PIV enabled.
    ui->containerViewPrivacyChecks->setVisible(false);
    onMintSelected(false);

    ui->btnTotalzPIV->setTitleClassAndText("btn-title-grey", "Total 0 zPIV");
    ui->btnTotalzPIV->setSubTitleClassAndText("text-subtitle", "Show denominations of zPIV owned.");
    ui->btnTotalzPIV->setRightIconClass("ic-arrow");

    ui->btnCoinControl->setTitleClassAndText("btn-title-grey", "Coin Control");
    ui->btnCoinControl->setSubTitleClassAndText("text-subtitle", "Select PIV outputs to mint into zPIV.");

    ui->btnDenomGeneration->setTitleClassAndText("btn-title-grey", "Denom Generation");
    ui->btnDenomGeneration->setSubTitleClassAndText("text-subtitle", "Select the denomination of the coins.");
    ui->btnDenomGeneration->setVisible(false);

    ui->btnRescanMints->setTitleClassAndText("btn-title-grey", "Rescan Mints");
    ui->btnRescanMints->setSubTitleClassAndText("text-subtitle", "Find mints in the blockchain.");

    ui->btnResetZerocoin->setTitleClassAndText("btn-title-grey", "Reset Zerocoin");
    ui->btnResetZerocoin->setSubTitleClassAndText("text-subtitle", "Reset zerocoin database.");

    connect(ui->btnTotalzPIV, SIGNAL(clicked()), this, SLOT(onTotalZpivClicked()));
    connect(ui->btnCoinControl, SIGNAL(clicked()), this, SLOT(onCoinControlClicked()));
    connect(ui->btnDenomGeneration, SIGNAL(clicked()), this, SLOT(onDenomClicked()));
    connect(ui->btnRescanMints, SIGNAL(clicked()), this, SLOT(onRescanMintsClicked()));
    connect(ui->btnResetZerocoin, SIGNAL(clicked()), this, SLOT(onResetZeroClicked()));

    ui->pushRight->setChecked(true);
    connect(ui->pushLeft, &QPushButton::clicked, [this](){onMintSelected(false);});
    connect(ui->pushRight,  &QPushButton::clicked, [this](){onMintSelected(true);});

    // List
    setCssProperty(ui->listView, "container");
    txHolder = new TxViewHolder(isLightTheme());
    delegate = new FurAbstractListItemDelegate(
                DECORATION_SIZE,
                txHolder,
                this
    );

    ui->listView->setItemDelegate(delegate);
    ui->listView->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listView->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listView->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->listView->setSelectionBehavior(QAbstractItemView::SelectRows);
}

void ColdStakingWidget::loadWalletModel(){
    if(walletModel) {
        txModel = walletModel->getTransactionTableModel();
        // Set up transaction list
        filter = new TransactionFilterProxy();
        filter->setDynamicSortFilter(true);
        filter->setSortCaseSensitivity(Qt::CaseInsensitive);
        filter->setFilterCaseSensitivity(Qt::CaseInsensitive);
        filter->setSortRole(Qt::EditRole);
        filter->setShowZcTxes(true);
        filter->setSourceModel(txModel);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);
        txHolder->setDisplayUnit(walletModel->getOptionsModel()->getDisplayUnit());
        txHolder->setFilter(filter);
        ui->listView->setModel(filter);

        updateDisplayUnit();
        updateDenomsSupply();

        if (!txModel->hasZcTxes()) {
            ui->emptyContainer->setVisible(true);
            ui->listView->setVisible(false);
        }else{
            showList();
        }

        connect(ui->pushButtonSave, SIGNAL(clicked()), this, SLOT(onSendClicked()));
    }

}

void ColdStakingWidget::onMintSelected(bool isMint){
    QString btnText;
    if(isMint){
        btnText = tr("Mint zPIV");
        ui->btnCoinControl->setVisible(true);
        ui->labelSubtitleAmount->setText(tr("Enter amount of PIV to mint into zPIV"));
    }else{
        btnText = tr("Convert back to PIV");
        ui->btnCoinControl->setVisible(false);
        ui->labelSubtitleAmount->setText(tr("Enter amount of zPIV to convert back into PIV"));
    }
    ui->pushButtonSave->setText(btnText);
}

void ColdStakingWidget::updateDisplayUnit() {
    if (walletModel && walletModel->getOptionsModel()) {
        nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();

        txHolder->setDisplayUnit(nDisplayUnit);
        ui->listView->update();
    }
}

void ColdStakingWidget::showList(){
    ui->emptyContainer->setVisible(false);
    ui->listView->setVisible(true);
}

void ColdStakingWidget::onTotalZpivClicked(){
    bool isVisible = ui->layoutDenom->isVisible();
    if(!isVisible){
        ui->layoutDenom->setVisible(true);
        ui->btnTotalzPIV->setRightIconClass("btn-dropdown", true);
    }else{
        ui->layoutDenom->setVisible(false);
        ui->btnTotalzPIV->setRightIconClass("ic-arrow", true);
    }
}

void ColdStakingWidget::onSendClicked(){
    if (!walletModel || !walletModel->getOptionsModel())
        return;

    if(GetAdjustedTime() > GetSporkValue(SPORK_16_ZEROCOIN_MAINTENANCE_MODE)) {
        warn(tr("Zerocoin"), tr("zPIV is currently undergoing maintenance"));
        return;
    }

    // Only convert enabled.
    bool isConvert = true;// ui->pushLeft->isChecked();

    if(!GUIUtil::requestUnlock(walletModel, AskPassphraseDialog::Context::Mint_zPIV, true)){
        inform(tr("You need to unlock the wallet to be able to %1 zPIV").arg(isConvert ? tr("convert") : tr("mint")));
        return;
    }

    bool isValid = true;
    CAmount value = GUIUtil::parseValue(
            ui->lineEditAmount->text(),
            walletModel->getOptionsModel()->getDisplayUnit(),
            &isValid
    );

    if (!isValid || value <= 0) {
        setCssEditLine(ui->lineEditAmount, false, true);
        inform(tr("Invalid value"));
        return;
    }

    setCssEditLine(ui->lineEditAmount, true, true);
    if(isConvert){
        spend(value);
    }else{
        mint(value);
    }
}

void ColdStakingWidget::mint(CAmount value){
    std::string strError;
    if(!walletModel->mintCoins(value, CoinControlDialog::coinControl, strError)){
        inform(tr(strError.data()));
    }else{
        // Mint succeed
        inform(tr("zPIV minted successfully"));
        // clear
        ui->lineEditAmount->clear();
    }
}

void ColdStakingWidget::spend(CAmount value){
    CZerocoinSpendReceipt receipt;
    std::vector<CZerocoinMint> selectedMints;
    bool mintChange = false;
    bool minimizeChange = false;

    if(!walletModel->convertBackZpiv(
            value,
            selectedMints,
            mintChange,
            minimizeChange,
            receipt,
            walletModel->getNewAddress()
    )){
        inform(receipt.GetStatusMessage().data());
    }else{
        // Spend succeed
        inform(tr("zPIV converted back to PIV"));
        // clear
        ui->lineEditAmount->clear();
    }
}


void ColdStakingWidget::onCoinControlClicked(){
    if(ui->pushRight->isChecked()) {
        if (walletModel->getBalance() > 0) {
            if (!coinControlDialog) {
                coinControlDialog = new CoinControlDialog();
                coinControlDialog->setModel(walletModel);
            }
            coinControlDialog->exec();
            ui->btnCoinControl->setActive(CoinControlDialog::coinControl->HasSelected());
        } else {
            inform(tr("You don't have any PIV to select."));
        }
    }
}

void ColdStakingWidget::onDenomClicked(){
    showHideOp(true);
    DenomGenerationDialog* dialog = new DenomGenerationDialog(window);
    openDialogWithOpaqueBackgroundY(dialog, window, 4.5, 5);
}

void ColdStakingWidget::onRescanMintsClicked(){
    if (ask(tr("Rescan Mints"),
        tr("Your zerocoin mints are going to be scanned from the blockchain from scratch"))
    ){
        std::string strResetMintResult = walletModel->resetMintZerocoin();
        inform(QString::fromStdString(strResetMintResult));
    }
}

void ColdStakingWidget::onResetZeroClicked(){
    if (ask(tr("Reset Spent Zerocoins"),
        tr("Your zerocoin spends are going to be scanned from the blockchain from scratch"))
    ){
        std::string strResetMintResult = walletModel->resetSpentZerocoin();
        inform(QString::fromStdString(strResetMintResult));
    }
}

void ColdStakingWidget::updateDenomsSupply(){
    std::map<libzerocoin::CoinDenomination, CAmount> mapDenomBalances;
    std::map<libzerocoin::CoinDenomination, int> mapUnconfirmed;
    std::map<libzerocoin::CoinDenomination, int> mapImmature;
    for (const auto& denom : libzerocoin::zerocoinDenomList){
        mapDenomBalances.insert(std::make_pair(denom, 0));
        mapUnconfirmed.insert(std::make_pair(denom, 0));
        mapImmature.insert(std::make_pair(denom, 0));
    }

    std::set<CMintMeta> vMints;
    walletModel->listZerocoinMints(vMints, true, false, true, true);

    std::map<libzerocoin::CoinDenomination, int> mapMaturityHeights = GetMintMaturityHeight();
    for (auto& meta : vMints){
        // All denominations
        mapDenomBalances.at(meta.denom)++;

        if (!meta.nHeight || chainActive.Height() - meta.nHeight <= Params().Zerocoin_MintRequiredConfirmations()) {
            // All unconfirmed denominations
            mapUnconfirmed.at(meta.denom)++;
        } else {
            if (meta.denom == libzerocoin::CoinDenomination::ZQ_ERROR) {
                mapImmature.at(meta.denom)++;
            } else if (meta.nHeight >= mapMaturityHeights.at(meta.denom)) {
                mapImmature.at(meta.denom)++;
            }
        }
    }

    int64_t nCoins = 0;
    int64_t nSumPerCoin = 0;
    int64_t nUnconfirmed = 0;
    int64_t nImmature = 0;
    QString strDenomStats, strUnconfirmed = "";

    for (const auto& denom : libzerocoin::zerocoinDenomList) {
        nCoins = libzerocoin::ZerocoinDenominationToInt(denom);
        nSumPerCoin = nCoins * mapDenomBalances.at(denom);
        nUnconfirmed = mapUnconfirmed.at(denom);
        nImmature = mapImmature.at(denom);

        strUnconfirmed = "";
        if (nUnconfirmed) {
            strUnconfirmed += QString::number(nUnconfirmed) + QString(" unconf. ");
        }
        if(nImmature) {
            strUnconfirmed += QString::number(nImmature) + QString(" immature ");
        }
        if(nImmature || nUnconfirmed) {
            strUnconfirmed = QString("( ") + strUnconfirmed + QString(") ");
        }

        strDenomStats = strUnconfirmed + QString::number(mapDenomBalances.at(denom)) + " x " +
                        QString::number(nCoins) + " = <b>" +
                        QString::number(nSumPerCoin) + " zPIV </b>";

        switch (nCoins) {
            case libzerocoin::CoinDenomination::ZQ_ONE:
                ui->labelValueDenom1->setText(strDenomStats);
                break;
            case libzerocoin::CoinDenomination::ZQ_FIVE:
                ui->labelValueDenom5->setText(strDenomStats);
                break;
            case libzerocoin::CoinDenomination::ZQ_TEN:
                ui->labelValueDenom10->setText(strDenomStats);
                break;
            case libzerocoin::CoinDenomination::ZQ_FIFTY:
                ui->labelValueDenom50->setText(strDenomStats);
                break;
            case libzerocoin::CoinDenomination::ZQ_ONE_HUNDRED:
                ui->labelValueDenom100->setText(strDenomStats);
                break;
            case libzerocoin::CoinDenomination::ZQ_FIVE_HUNDRED:
                ui->labelValueDenom500->setText(strDenomStats);
                break;
            case libzerocoin::CoinDenomination::ZQ_ONE_THOUSAND:
                ui->labelValueDenom1000->setText(strDenomStats);
                break;
            case libzerocoin::CoinDenomination::ZQ_FIVE_THOUSAND:
                ui->labelValueDenom5000->setText(strDenomStats);
                break;
            default:
                // Error Case: don't update display
                break;
        }
    }

    CAmount matureZerocoinBalance = walletModel->getZerocoinBalance() - walletModel->getUnconfirmedZerocoinBalance() - walletModel->getImmatureZerocoinBalance();
    ui->btnTotalzPIV->setTitleText(tr("Total %1").arg(GUIUtil::formatBalance(matureZerocoinBalance, nDisplayUnit, true)));
}

void ColdStakingWidget::changeTheme(bool isLightTheme, QString& theme){
    static_cast<TxViewHolder*>(this->delegate->getRowFactory())->isLightTheme = isLightTheme;
    ui->listView->update();
}

ColdStakingWidget::~ColdStakingWidget(){
    delete ui;
}
