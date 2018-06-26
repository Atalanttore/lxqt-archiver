#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "archiver.h"
#include "archiveritem.h"
#include "archiverproxymodel.h"
#include "core/file-utils.h"

#include <QFileDialog>
#include <QUrl>
#include <QStandardItemModel>
#include <QIcon>
#include <QCursor>
#include <QHeaderView>
#include <QTreeView>
#include <QMessageBox>
#include <QDateTime>
#include <QProgressBar>
#include <QLabel>
#include <QTextCodec>
#include <QActionGroup>
#include <QLineEdit>
#include <QToolBar>
#include <QMenu>

#include <QDebug>

#include <libfm-qt/core/mimetype.h>
#include <libfm-qt/core/iconinfo.h>
#include <libfm-qt/core/gioptrs.h>
#include <libfm-qt/utilities.h>
// #include <libfm-qt/pathbar.h>

#include <map>


MainWindow::MainWindow(QWidget* parent):
    QMainWindow(parent),
    ui_{new Ui::MainWindow()},
    archiver_{std::make_shared<Archiver>()},
    viewMode_{ViewMode::DirTree},
    currentDirItem_{nullptr} {

    ui_->setupUi(this);

    // only stretch the right pane
    ui_->splitter->setStretchFactor(0, 0);
    ui_->splitter->setStretchFactor(1, 1);

    // create a progress bar in the status bar
    progressBar_ = new QProgressBar{ui_->statusBar};
    ui_->statusBar->addPermanentWidget(progressBar_);
    progressBar_->hide();

    // view menu
    auto viewModeGroup = new QActionGroup{this};
    viewModeGroup->addAction(ui_->actionDirTreeMode);
    viewModeGroup->addAction(ui_->actionFlatListMode);

    // FIXME: need to add a way in libfm-qt to turn off default auto-complete
#if 0
    auto pathBar = new Fm::PathBar{this};
    pathBar->setPath(Fm::FilePath::fromLocalPath("/"));
    ui_->toolBar->addWidget(pathBar);
#endif
    currentPathEdit_ = new QLineEdit(this);
    ui_->toolBar->addWidget(currentPathEdit_);

    popupMenu_ = new QMenu{this};
    popupMenu_->addAction(ui_->actionExtract);
    popupMenu_->addAction(ui_->actionDelete);

    // proxy model used to filter and sort the items
    proxyModel_ = new ArchiverProxyModel{this};
    proxyModel_->setFolderFirst(true);
    proxyModel_->setSortLocaleAware(true);
    proxyModel_->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxyModel_->setSortRole(Qt::DisplayRole);
    proxyModel_->sort(0, Qt::AscendingOrder);

    ui_->fileListView->setModel(proxyModel_);
    connect(ui_->fileListView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::onFileListSelectionChanged);
    connect(ui_->fileListView, &QAbstractItemView::activated, this, &MainWindow::onFileListActivated);

    // show context menu
    ui_->fileListView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui_->fileListView, &QAbstractItemView::customContextMenuRequested, this, &MainWindow::onFileListContextMenu);

    connect(archiver_.get(), &Archiver::invalidateContent, this, &MainWindow::onInvalidateContent);
    connect(archiver_.get(), &Archiver::start, this, &MainWindow::onActionStarted);
    connect(archiver_.get(), &Archiver::finish, this, &MainWindow::onActionFinished);
    connect(archiver_.get(), &Archiver::progress, this, &MainWindow::onActionProgress);
    connect(archiver_.get(), &Archiver::message, this, &MainWindow::onMessage);

    updateUiStates();

    // hide stuff we don't support yet
    // FIXME: implement the missing features
    ui_->actionSaveAs->deleteLater();
    ui_->actionArchiveProperties->deleteLater();
    ui_->actionCut->deleteLater();
    ui_->actionCopy->deleteLater();
    ui_->actionPaste->deleteLater();
    ui_->actionRename->deleteLater();
    ui_->actionFind->deleteLater();
    ui_->actionPassword->deleteLater(); // FIXME: need to implement first
}

MainWindow::~MainWindow() {
}

void MainWindow::setFileName(const QString &fileName) {
    QString title = tr("File Archiver");
    if(!fileName.isEmpty()) {
        title = fileName + " - " + title;
    }
    setWindowTitle(title);
}

void MainWindow::on_actionCreateNew_triggered(bool checked) {
    QFileDialog dlg;
    QUrl url = QFileDialog::getSaveFileUrl(this);
    if(!url.isEmpty()) {
        archiver_->createNewArchive(url);
    }
}

void MainWindow::on_actionOpen_triggered(bool checked) {
    qDebug("open");
    QFileDialog dlg;
    QUrl url = QFileDialog::getOpenFileUrl(this);
    if(!url.isEmpty()) {
        auto uri = url.toString().toUtf8();
        archiver_->openArchive(uri.constData(), nullptr);
    }
}

void MainWindow::on_actionArchiveProperties_triggered(bool checked) {
    // TODO: show file properties
}

void MainWindow::on_actionAddFiles_triggered(bool checked) {
    auto fileUrls = QFileDialog::getOpenFileUrls(this);
    if(!fileUrls.isEmpty()) {
        auto srcPaths = Fm::pathListFromQUrls(fileUrls);
        const char* baseDir;
        if(viewMode_ == ViewMode::DirTree && currentDirItem_) {
            baseDir = currentDirItem_->originalPath();
        }
        else {
            baseDir = "/";
        }
        archiver_->addFiles(srcPaths, baseDir,
                            false, nullptr, false, FR_COMPRESSION_NORMAL, 0);
    }
}

void MainWindow::on_actionAddFolder_triggered(bool checked) {
    QFileDialog dlg;
    QUrl dirUrl = QFileDialog::getExistingDirectoryUrl(this, QString(), QUrl(), (QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog));
    if(!dirUrl.isEmpty()) {
        auto path = Fm::FilePath::fromUri(dirUrl.toEncoded().constData());
        const char* baseDir;
        if(viewMode_ == ViewMode::DirTree && currentDirItem_) {
            baseDir = currentDirItem_->originalPath();
        }
        else {
            baseDir = "/";
        }
        archiver_->addDirectory(path, baseDir,
                                false, nullptr, false, FR_COMPRESSION_NORMAL, 0);
    }
}

void MainWindow::on_actionDelete_triggered(bool checked) {
    if(QMessageBox::question(this, tr("Confirm"), tr("Are you sure you want to delete selected files?"), QMessageBox::Yes|QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    qDebug("delete");
    auto files = selectedFiles();
    if(!files.empty()) {
        archiver_->removeFiles(files, FR_COMPRESSION_NORMAL);
    }
}

void MainWindow::on_actionSelectAll_triggered(bool checked) {
    ui_->fileListView->selectAll();
}

void MainWindow::on_actionExtract_triggered(bool checked) {
    qDebug("extract");
    QFileDialog dlg;
    QUrl dirUrl = QFileDialog::getExistingDirectoryUrl(this, QString(), QUrl(), (QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog));
    if(!dirUrl.isEmpty()) {
        auto files = selectedFiles();
        if(files.empty()) {
            archiver_->extractAll(dirUrl.toEncoded().constData(), false, false, false, nullptr);
        }
        else {
            // TODO: refer to fr_window_get_selection() in fr-window.c of engrampa to see how this is implemented.
            // fr_window_go_to_location() to see the format of current location (must be ends with '/')
            auto destDir = Fm::FilePath::fromUri(dirUrl.toEncoded().constData());
            auto baseDir = currentDirPath_;
            if(baseDir.empty() || baseDir.back() != '/') {
                baseDir += '/';
            }
            archiver_->extractFiles(files, destDir, currentDirPath_.c_str(), false, false, false, nullptr);
        }
    }
}

void MainWindow::on_actionTest_triggered(bool checked) {
    if(archiver_->isLoaded()) {
        archiver_->testArchiveIntegrity(nullptr);
    }
}

void MainWindow::on_actionDirTree_toggled(bool checked) {
    bool visible = checked && viewMode_ == ViewMode::DirTree;
    ui_->dirTreeView->setVisible(visible);
}

void MainWindow::on_actionDirTreeMode_toggled(bool checked) {
    setViewMode(ViewMode::DirTree);
}

void MainWindow::on_actionFlatListMode_toggled(bool checked) {
    setViewMode(ViewMode::FlatList);
}

void MainWindow::on_actionReload_triggered(bool checked) {
    if(archiver_->isLoaded()) {
        archiver_->reloadArchive(nullptr);
    }
}

void MainWindow::on_actionAbout_triggered(bool checked) {
    QMessageBox::about(this, tr("About LXQt Archiver"), tr("File Archiver for LXQt.\n\nCopyright (C) 2018 LXQt team."));
}

void MainWindow::onDirTreeSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected) {
    auto selModel = ui_->dirTreeView->selectionModel();
    auto selectedRows = selModel->selectedRows();
    if(!selectedRows.isEmpty()) {
        // update current dir
        auto idx = selectedRows[0];
        auto dir = itemFromIndex(idx);

        chdir(dir);

        // expand the node as needed
        ui_->dirTreeView->expand(idx);
    }
}

void MainWindow::onFileListSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected) {
}

void MainWindow::onFileListContextMenu(const QPoint &pos) {
    // QAbstractScrollArea and its subclasses map the context menu event to coordinates of the viewport().
    auto globalPos = ui_->fileListView->viewport()->mapToGlobal(pos);
    popupMenu_->popup(globalPos);
}

void MainWindow::onFileListActivated(const QModelIndex &index) {
    auto item = itemFromIndex(index);
    if(item && item->isDir()) {
        chdir(item);
    }
}

void MainWindow::onInvalidateContent() {
    // clear all models and make sure we don't cache any FileData pointers
    auto oldModel = proxyModel_->sourceModel();
    proxyModel_->setSourceModel(nullptr);
    if(oldModel) {
        delete oldModel;
    }

    currentDirItem_ = nullptr;
}

void MainWindow::onActionStarted(FrAction action) {
    setBusyState(true);
    progressBar_->setValue(0);
    progressBar_->show();
    progressBar_->setRange(0, 100);
    progressBar_->setFormat(tr("%p %"));

    qDebug("action start: %d", action);

    switch(action) {
    case FR_ACTION_CREATING_NEW_ARCHIVE:
        qDebug("new archive");
        setFileName(archiver_->archiveDisplayName());
        break;
    case FR_ACTION_LOADING_ARCHIVE:            /* loading the archive from a remote location */
        setFileName(archiver_->archiveDisplayName());
        break;
    case FR_ACTION_LISTING_CONTENT:            /* listing the content of the archive */
        setFileName(archiver_->archiveDisplayName());
        break;
    case FR_ACTION_DELETING_FILES:             /* deleting files from the archive */
        break;
    case FR_ACTION_TESTING_ARCHIVE:            /* testing the archive integrity */
        break;
    case FR_ACTION_GETTING_FILE_LIST:          /* getting the file list (when fr_archive_add_with_wildcard or
                         fr_archive_add_directory are used, we need to scan a directory
                         and collect the files to add to the archive, this
                         may require some time to complete, so the operation
                         is asynchronous) */
        break;
    case FR_ACTION_COPYING_FILES_FROM_REMOTE:  /* copying files to be added to the archive from a remote location */
        break;
    case FR_ACTION_ADDING_FILES:    /* adding files to an archive */
        break;
    case FR_ACTION_EXTRACTING_FILES:           /* extracting files */
        break;
    case FR_ACTION_COPYING_FILES_TO_REMOTE:    /* copying extracted files to a remote location */
        break;
    case FR_ACTION_CREATING_ARCHIVE:           /* creating a local archive */
        break;
    case FR_ACTION_SAVING_REMOTE_ARCHIVE:       /* copying the archive to a remote location */
        break;
    default:
        break;
    }
}

void MainWindow::onActionProgress(double fraction) {
    progressBar_->setValue(int(100 * fraction));
}

void MainWindow::onActionFinished(FrAction action, ArchiverError err) {
    setBusyState(false);
    progressBar_->hide();

    qDebug("action finished: %d", action);

    switch(action) {
    case FR_ACTION_LOADING_ARCHIVE:            /* loading the archive from a remote location */
        qDebug("finish! %d", action);
        break;
    case FR_ACTION_CREATING_NEW_ARCHIVE:  // same as listing empty content
    case FR_ACTION_CREATING_ARCHIVE:           /* creating a local archive */
    case FR_ACTION_LISTING_CONTENT:            /* listing the content of the archive */
        qDebug("content listed");
        // content dir list of the archive is fully loaded
        updateDirTree();

        // try to see if the previous current dir path is still valid
        currentDirItem_ = archiver_->dirByPath(currentDirPath_.c_str());
        if(!currentDirItem_) {
            currentDirItem_ = archiver_->dirTreeRoot();
        }
        chdir(currentDirItem_);

        break;
    case FR_ACTION_DELETING_FILES:             /* deleting files from the archive */
        archiver_->reloadArchive(nullptr);
        break;
    case FR_ACTION_TESTING_ARCHIVE:            /* testing the archive integrity */
        if(!err.hasError()) {
            QMessageBox::information(this, tr("Success"), tr("No errors are found."));
        }
        break;
    case FR_ACTION_GETTING_FILE_LIST:          /* getting the file list (when fr_archive_add_with_wildcard or
                         fr_archive_add_directory are used, we need to scan a directory
                         and collect the files to add to the archive, this
                         may require some time to complete, so the operation
                         is asynchronous) */
        break;
    case FR_ACTION_COPYING_FILES_FROM_REMOTE:  /* copying files to be added to the archive from a remote location */
        break;
    case FR_ACTION_ADDING_FILES:    /* adding files to an archive */
        archiver_->reloadArchive(nullptr);
        break;
    case FR_ACTION_EXTRACTING_FILES:           /* extracting files */
        break;
    case FR_ACTION_COPYING_FILES_TO_REMOTE:    /* copying extracted files to a remote location */
        break;
    case FR_ACTION_SAVING_REMOTE_ARCHIVE:       /* copying the archive to a remote location */
        break;
    default:
        break;
    }

    if(err.hasError()) {
        QMessageBox::critical(this, tr("Error"), err.message());
    }
}

void MainWindow::onMessage(QString message) {
    ui_->statusBar->showMessage(message);
}

void MainWindow::onStoppableChanged(bool stoppable) {
    ui_->actionStop->setEnabled(stoppable);
}

QList<QStandardItem *> MainWindow::createFileListRow(const ArchiverItem *file) {
    QIcon icon;
    QString desc;
    // get mime type, icon, and description
    auto typeName = file->contentType();
    auto mimeType = Fm::MimeType::fromName(typeName);
    if(mimeType) {
        auto iconInfo = mimeType->icon();
        desc = QString::fromUtf8(mimeType->desc());
        if(iconInfo) {
            icon = iconInfo->qicon();
        }
    }

    // mtime
    auto mtime = QDateTime::fromMSecsSinceEpoch(file->modifiedTime() * 1000);

    // FIXME: filename might not be UTF-8
    QString name = viewMode_ == ViewMode::FlatList ? file->fullPath() : file->name();
    auto nameItem = new QStandardItem{icon, name};
    nameItem->setData(QVariant::fromValue(file), ArchiverItemRole); // store the item pointer on the first column
    nameItem->setEditable(false);

    auto descItem = new QStandardItem{desc};
    descItem->setEditable(false);

    auto sizeItem = new QStandardItem{Fm::formatFileSize(file->size())};
    sizeItem->setEditable(false);

    auto mtimeItem = new QStandardItem{mtime.toString(Qt::SystemLocaleShortDate)};
    mtimeItem->setEditable(false);

    auto encryptedItem = new QStandardItem{file->isEncrypted() ? QStringLiteral("*") : QString{}};
    encryptedItem->setEditable(false);

    return QList<QStandardItem*>() << nameItem << descItem << sizeItem << mtimeItem << encryptedItem;
}

// show all files including files in subdirs in a flat list
void MainWindow::showFileList(const std::vector<const ArchiverItem *> &files) {
    auto oldModel = proxyModel_->sourceModel();

    QStandardItemModel* model = new QStandardItemModel{this};
    model->setHorizontalHeaderLabels(QStringList()
                                     << tr("File name")
                                     << tr("File Type")
                                     << tr("File Size")
                                     << tr("Modified")
                                     << tr("*")
    );

    if(viewMode_ == ViewMode::DirTree) {
        // add ".." for the parent dir if it's not roots
        if(currentDirItem_) {
            auto parent = archiver_->parentDir(currentDirItem_);
            if(parent) {
                qDebug("parent: %s", parent ? parent->fullPath() : "null");
                auto parentRow = createFileListRow(parent);
                parentRow[0]->setText("..");
                model->appendRow(parentRow);
            }
        }
    }

    for(const auto& file: files) {
        model->appendRow(createFileListRow(file));
    }

    if(oldModel) {
        // Workaround for Qt 5.11 QSortFilterProxyModel bug: https://bugreports.qt.io/browse/QTBUG-68581
#if QT_VERSION == QT_VERSION_CHECK(5, 11, 0)
        oldModel->disconnect(this);
#endif
        delete oldModel;
    }

    proxyModel_->setSourceModel(model);

    ui_->statusBar->showMessage(tr("%1 files").arg(files.size()));
    // ui_->fileListView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui_->fileListView->resizeColumnToContents(0);
}

void MainWindow::showFlatFileList() {
    showFileList(archiver_->flatFileList());
}

void MainWindow::showCurrentDirList() {
    auto dir = currentDirItem_ ? currentDirItem_ : archiver_->dirTreeRoot();
    if(dir) {
        showFileList(dir->children());
    }
}

void MainWindow::setBusyState(bool busy) {
    if(busy) {
        setCursor(Qt::WaitCursor);
    }
    else {
        setCursor(Qt::ArrowCursor);
    }
    updateUiStates();
}

void MainWindow::updateUiStates() {
    bool hasArchive = archiver_->isLoaded();
    bool inProgress = archiver_->isBusy();

    bool canLoad = !hasArchive || !inProgress;
    ui_->actionCreateNew->setEnabled(canLoad);
    ui_->actionOpen->setEnabled(canLoad);

    bool canEdit = hasArchive && !inProgress;
    currentPathEdit_->setEnabled(canEdit);
    ui_->fileListView->setEnabled(canEdit);
    ui_->dirTreeView->setEnabled(canEdit);
    // FIXME support this later
    // ui_->actionSaveAs->setEnabled(canEdit);

    ui_->actionSelectAll->setEnabled(canEdit);

    ui_->actionAddFiles->setEnabled(canEdit);
    ui_->actionAddFolder->setEnabled(canEdit);
    ui_->actionDelete->setEnabled(canEdit);

    ui_->actionExtract->setEnabled(canEdit);
}

std::vector<const FileData*> MainWindow::selectedFiles() {
    std::vector<const FileData*> files;
    // FIXME: use ArchiverItem instead of FileData
    auto selModel = ui_->fileListView->selectionModel();
    if(selModel) {
        auto selIndexes = selModel->selectedRows();
        for(const auto& idx: selIndexes) {
            auto item = itemFromIndex(idx);
            qDebug("selected item: %p", item);
            if(item && item->data()) {
                files.emplace_back(item->data());
            }
        }
    }
    return files;
}

const ArchiverItem *MainWindow::itemFromIndex(const QModelIndex &index) {
    if(index.isValid()) {
        // get first column of the current row
        auto firstCol = index.sibling(index.row(), 0);
        return firstCol.data(ArchiverItemRole).value<const ArchiverItem*>();
    }
    return nullptr;
}

QModelIndex MainWindow::indexFromItem(const QModelIndex &parent, const ArchiverItem* item) {
    QModelIndex index;
    if(parent.isValid()) {
        auto model = parent.model();
        auto n_rows = model->rowCount(parent);
        for(int row = 0; row < n_rows; ++row) {
            auto rowIdx = parent.child(row, 0);
            if(itemFromIndex(rowIdx) == item) {
                index = std::move(rowIdx);
                break;
            }
            else if(model->hasChildren(rowIdx)) {  // see if we need to search recursively
                auto childIdx = indexFromItem(rowIdx, item);
                if(childIdx.isValid()) {
                    index = childIdx;
                    break;
                }
            }
        }
    }
    return index;
}

void MainWindow::updateDirTree() {
    // update the dir tree view at left pane

    // delete old model
    // disconnect(ui_->dirTreeView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::onDirTreeSelectionChanged);
    auto oldModel = ui_->dirTreeView->model();
    if(oldModel) {
        delete oldModel;
    }

    // build tree items
    auto treeRoot = archiver_->dirTreeRoot();
    QStandardItemModel* model = new QStandardItemModel{this};
    buildDirTree(model->invisibleRootItem(), treeRoot);
    ui_->dirTreeView->setModel(model);
    ui_->dirTreeView->expand(model->index(0, 0));

    // replace the icon & text of the root item with that of the whole archive
    // FIXME: name might not be UTF-8
    auto archivePath = archiver_->currentArchivePath();
    auto contentType = archiver_->currentArchiveContentType();
    auto rootItem = model->item(0, 0);
    rootItem->setText(archivePath.baseName().get());
    if(contentType) {
        auto mimeType = Fm::MimeType::fromName(contentType);
        if(mimeType && mimeType->icon()) {
            rootItem->setIcon(mimeType->icon()->qicon());
        }
    }
    connect(ui_->dirTreeView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::onDirTreeSelectionChanged);
}

void MainWindow::buildDirTree(QStandardItem *parent, const ArchiverItem *root) {
    if(root) {
        // FIXME: cache this
        auto iconInfo = Fm::MimeType::inodeDirectory()->icon();
        QIcon qicon = iconInfo ? iconInfo->qicon() : QIcon();

        // FIXME: root->name() might not be UTF-8
        auto item = new QStandardItem{qicon, root->name()};

        item->setEditable(false);
        item->setData(QVariant::fromValue(root), ArchiverItemRole);
        parent->appendRow(QList<QStandardItem*>() << item);
        for(auto child: root->children()) {
            if(child->isDir()) {
                buildDirTree(item, child);
            }
        }
    }
}

const std::string &MainWindow::currentDirPath() const {
    return currentDirPath_;
}

void MainWindow::chdir(std::string dirPath) {
    if(dirPath != currentDirPath_) {
        auto dir = archiver_->dirByPath(currentDirPath_.c_str());
        if(dir) {
            chdir(dir);
        }
        else {
            // TODO: show error message
        }
    }
}

void MainWindow::chdir(const ArchiverItem *dir) {
    currentDirPath_ = dir->fullPath();
    currentPathEdit_->setText(dir->fullPath());
    currentDirItem_ = dir;
    if(viewMode_ == ViewMode::DirTree) {
        showCurrentDirList();
    }
    else {
        showFlatFileList();
    }

    // also select this item in dir tree
    auto dirTreeModel = ui_->dirTreeView->model();
    if(dirTreeModel) {
        auto dirTreeIdx = indexFromItem(dirTreeModel->index(0, 0), currentDirItem_);
        if(dirTreeIdx.isValid()) {
            auto selModel = ui_->dirTreeView->selectionModel();
            selModel->select(dirTreeIdx, QItemSelectionModel::Select|QItemSelectionModel::Current);
            ui_->dirTreeView->scrollTo(dirTreeIdx);
        }
    }
}

MainWindow::ViewMode MainWindow::viewMode() const {
    return viewMode_;
}

void MainWindow::setViewMode(MainWindow::ViewMode viewMode) {
    if(viewMode_ != viewMode) {
        viewMode_ = viewMode;
        switch(viewMode) {
        case ViewMode::DirTree:
            ui_->dirTreeView->setVisible(ui_->actionDirTree->isChecked());
            showCurrentDirList();
            break;
        case ViewMode::FlatList:
            // always hide dir tree view in flast list mode
            ui_->dirTreeView->hide();
            showFlatFileList();
            break;
        }
    }
}

std::shared_ptr<Archiver> MainWindow::archiver() const {
    return archiver_;
}
