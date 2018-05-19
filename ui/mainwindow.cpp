#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "conversionrunnable.h"
#include "sourcecodegenerator.h"
#include <QDebug>
#include <QThreadPool>
#include <QFontDatabase>
#include <QElapsedTimer>
#include <QMessageBox>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setupSourceCodeGenerators();

    QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    fixedFont.setPointSize(13);
    ui->textBrowser->setFont(fixedFont);
    QFont infoFont = ui->infoLabel->font();
    infoFont.setPixelSize(32);
    ui->infoLabel->setFont(infoFont);

    connect(ui->widthLineEdit, SIGNAL(editingFinished()), this, SLOT(validateTextFieldInput()));
    connect(ui->heightLineEdit, SIGNAL(editingFinished()), this, SLOT(validateTextFieldInput()));

    connect(ui->topBottomRadioButton, SIGNAL(clicked(bool)), this, SLOT(updateConfig()));
    connect(ui->invertBitsCheckBox, SIGNAL(clicked(bool)), this, SLOT(updateConfig()));
    connect(ui->bitNumberingCheckBox, SIGNAL(clicked(bool)), this, SLOT(updateConfig()));
    connect(ui->formatComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(updateConfig()));

    connect(ui->stackedWidget, SIGNAL(dropActionAvailableChanged(bool)), this, SLOT(setDropActionAvailable(bool)));
    connect(ui->stackedWidget, SIGNAL(imageFileDropped(QUrl)), this, SLOT(loadImageFile(QUrl)));

    config.loadFromSettings();
}

MainWindow::~MainWindow()
{
    generators.clear();

    if (conversion && !conversion->isFinished()) {
        conversion->setCanceled(true);
        conversion = nullptr;
    }
    delete ui;
}

void MainWindow::setupSourceCodeGenerators()
{
    generators.clear();
    SourceCodeGeneratorItem cGenerator;
    cGenerator.title = tr("C/C++");
    cGenerator.createGenerator = [this]() {
        SourceCodeOptions options(this->config.bitNumbering, this->config.shouldInvertBits);
        return new CCodeGenerator(options);
    };
    generators << cGenerator;

    SourceCodeGeneratorItem arduinoGenerator;
    arduinoGenerator.title = tr("Arduino");
    arduinoGenerator.createGenerator = [this]() {
        SourceCodeOptions options = { this->config.bitNumbering, this->config.shouldInvertBits };
        return new ArduinoCodeGenerator(options);
    };
    generators << arduinoGenerator;

    for (auto generatorItem : generators) {
        ui->formatComboBox->addItem(generatorItem.title);
    }
}

void MainWindow::showEvent(QShowEvent *event)
{
    Q_UNUSED(event);
    applyCurrentConfig();
}

void MainWindow::updateConfig()
{
    bool converted = false;
    int intValue = ui->widthLineEdit->text().toInt(&converted);
    config.fontWidth = converted ? (uint8_t)intValue : 0;

    converted = false;
    intValue = ui->heightLineEdit->text().toInt(&converted);
    config.fontHeight = converted ? (uint8_t)intValue : 0;

    config.readingMode = ui->topBottomRadioButton->isChecked() ?
                FontConverter::TopToBottom :
                FontConverter::LeftToRight;

    config.shouldInvertBits = ui->invertBitsCheckBox->isChecked();

    config.bitNumbering = ui->bitNumberingCheckBox->isChecked() ?
                SourceCodeOptions::MSB :
                SourceCodeOptions::LSB;

    config.sourceCodeGeneratorIndex = ui->formatComboBox->currentIndex();

    config.saveToSettings();
}

void MainWindow::validateTextFieldInput()
{
    QLineEdit *lineEdit = qobject_cast<QLineEdit *>(sender());

    bool converted = false;
    int intValue = lineEdit->text().toInt(&converted);

    if (converted) {
        intValue = qMax(intValue, 1);
        intValue = qMin(intValue, 255);
        lineEdit->setText(QString::number(intValue));
    } else {
        lineEdit->setText(nullptr);
    }

    updateConfig();
    applyCurrentConfig();
}

void MainWindow::applyCurrentConfig()
{
    if (ui->stackedWidget->currentIndex() != TextBrowser) {
        if (config.isValid()) {
            ui->stackedWidget->setCurrentIndex(PromptLabel);
        } else {
            ui->stackedWidget->setCurrentIndex(InfoLabel);
        }
    }
    ui->stackedWidget->setAcceptDrops(config.isValid());

    QString fontString = config.isWidthValid() ? QString::number(config.fontWidth) : "";
    ui->widthLineEdit->setText(fontString);
    fontString = config.isHeightValid() ? QString::number(config.fontHeight) : "";
    ui->heightLineEdit->setText(fontString);

    switch (config.readingMode) {
    case FontConverter::TopToBottom:
        ui->topBottomRadioButton->setChecked(true);
        break;
    case FontConverter::LeftToRight:
        ui->leftRightRadioButton->setChecked(true);
        break;
    }

    ui->invertBitsCheckBox->setChecked(config.shouldInvertBits);
    ui->bitNumberingCheckBox->setChecked(config.bitNumbering == SourceCodeOptions::MSB);

    if (config.sourceCodeGeneratorIndex < ui->formatComboBox->count()
            and config.sourceCodeGeneratorIndex > 0)
    {
        ui->formatComboBox->setCurrentIndex(config.sourceCodeGeneratorIndex);
    }
}

void MainWindow::setDropActionAvailable(bool available)
{
    ui->promptLabel->setDropActionAvailable(available);
    if (available and ui->stackedWidget->currentIndex() == TextBrowser) {
        dropActionHidesTextBrowser = true;
        ui->stackedWidget->setCurrentIndex(PromptLabel);
    } else if (!available and dropActionHidesTextBrowser) {
        dropActionHidesTextBrowser = false;
        ui->stackedWidget->setCurrentIndex(TextBrowser);
    }
}

void MainWindow::loadImageFile(const QUrl &url)
{
    QImage image(url.path());
    if (image.isNull()) {
        qDebug() << "image is null ._.";
        QMessageBox::critical(this,
                              tr("Couldn't read image from provided file"),
                              tr("Couldn't read image from provided file"),
                              QMessageBox::Close);
    } else {
        qDebug() << image.size();
        if (conversion and !conversion->isFinished()) {
            conversion->setCanceled(true);
        }

        auto generator = generators.at(ui->formatComboBox->currentIndex()).createGenerator();

        conversion = new ConversionRunnable();
        conversion->imageConverter()->setImage(new InputQImage(image));
        conversion->imageConverter()->setSourceCodeGenerator(generator);
        conversion->imageConverter()->setConfig(config);

        connect(conversion->imageConverter(), SIGNAL(conversionFinished(QString,ConverterError)),
                this, SLOT(imageConverted(QString,ConverterError)),
                Qt::BlockingQueuedConnection);

        conversionTimer.start();
        QThreadPool::globalInstance()->start(conversion);
    }
}

void MainWindow::imageConverted(const QString &sourceCode, const ConverterError &error)
{
    qint64 conversionTime = conversionTimer.elapsed();
    conversionTimer.invalidate();
    ui->statusBar->showMessage(tr("%1x%2 font generated in %3ms")
                               .arg(QString::number(config.fontHeight),
                                    QString::number(config.fontWidth),
                                    QString::number(conversionTime)));
    Q_ASSERT(conversion->isFinished());
    qDebug() << __PRETTY_FUNCTION__;
    if (error == ConverterError::NoError) {
        ui->stackedWidget->setCurrentIndex(TextBrowser);
        ui->textBrowser->setText(sourceCode);
    } else {
        QMessageBox::critical(this,
                              QString::fromStdString(error.summary),
                              QString::fromStdString(error.description),
                              QMessageBox::Close);
    }

    conversion = nullptr;
}