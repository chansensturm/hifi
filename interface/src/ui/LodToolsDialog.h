//
//  LodToolsDialog.h
//  interface/src/ui
//
//  Created by Brad Hefta-Gaub on 7/19/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_LodToolsDialog_h
#define hifi_LodToolsDialog_h

#include <QDialog>
#include <QLabel>
#include <QSlider>

class QCheckBox;
class QDoubleSpinBox;

class LodToolsDialog : public QDialog {
    Q_OBJECT
public:
    // Sets up the UI
    LodToolsDialog(QWidget* parent);
    ~LodToolsDialog();
    
signals:
    void closed();

public slots:
    void reject();
    void sizeScaleValueChanged(int value);
    void boundaryLevelValueChanged(int value);
    void resetClicked(bool checked);
    void reloadSliders();
    void updateAvatarLODControls();
    void updateAvatarLODValues();

protected:

    // Emits a 'closed' signal when this dialog is closed.
    void closeEvent(QCloseEvent*);

private:
    QSlider* _lodSize;
    QSlider* _boundaryLevelAdjust;
    QCheckBox* _automaticAvatarLOD;
    QDoubleSpinBox* _avatarLODDecreaseFPS;
    QDoubleSpinBox* _avatarLODIncreaseFPS;
    QDoubleSpinBox* _avatarLOD;
    QLabel* _feedback;
};

#endif // hifi_LodToolsDialog_h
