/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

static const char* testRigJson = R"({
  "cameras" : [
    {
      "fov" : 1.57079632679,
      "id" : "cam0",
      "origin" : [
        0.25129196625630573,
        -0.2027353327116483,
        -0.06819627970305264
      ],
      "principal" : [
        1682.087530345614,
        1083.9460130625214
      ],
      "right" : [
        -0.4117104932537061,
        -0.20435377363790347,
        -0.8881069783222844
      ],
      "up" : [
        -0.5116346651909054,
        -0.7546218007109797,
        0.4108234502395264
      ],
      "forward" : [
        0.7541382095609399,
        -0.6235266418459334,
        -0.20613123923499066
      ],
      "focal" : [
        1115.081474346635,
        -1115.081474346635
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam1",
      "origin" : [
        0.18725690898789224,
        0.012690777761121276,
        -0.27142916975910686
      ],
      "principal" : [
        1653.0824605284936,
        1086.3336081389639
      ],
      "right" : [
        -0.465169032034787,
        0.8375697433861375,
        -0.2865217209914439
      ],
      "up" : [
        -0.6891230513466808,
        -0.5457902984993526,
        -0.47667847671845576
      ],
      "forward" : [
        0.5556322450492405,
        0.02428734296021462,
        -0.8310733621248327
      ],
      "focal" : [
        1111.7991012579612,
        -1111.7991012579612
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam2",
      "origin" : [
        -0.014546065765626877,
        -0.17364816235984792,
        -0.2802404818718915
      ],
      "principal" : [
        1684.5821433111457,
        1078.832273085706
      ],
      "right" : [
        0.901476938165712,
        0.33841127568900187,
        -0.26984650904155766
      ],
      "up" : [
        -0.4297288388068209,
        0.7742490023561588,
        -0.4646198526195736
      ],
      "forward" : [
        -0.05169579337929227,
        -0.5348049091370004,
        -0.8433927045628569
      ],
      "focal" : [
        1113.430753776573,
        -1113.430753776573
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam3",
      "origin" : [
        -0.25998152272920955,
        -0.16793447550964388,
        -0.11448851372387696
      ],
      "principal" : [
        1698.4474557297503,
        1073.3913354145805
      ],
      "right" : [
        0.22720963696042323,
        0.29052385913508105,
        -0.9295007628536797
      ],
      "up" : [
        -0.5806100439203888,
        0.8066878905333255,
        0.11021172426587761
      ],
      "forward" : [
        -0.7818361450912058,
        -0.5146363128852776,
        -0.3519683333607868
      ],
      "focal" : [
        1111.4581410781072,
        -1111.4581410781072
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam4",
      "origin" : [
        -0.22311225382342434,
        -0.14856962873440527,
        0.1924785380543838
      ],
      "principal" : [
        1666.184786651635,
        1063.5141405997965
      ],
      "right" : [
        -0.005315425103010751,
        0.7578356878982763,
        0.652423801224113
      ],
      "up" : [
        0.7332348932660661,
        -0.44067742310483793,
        0.5178513300772533
      ],
      "forward" : [
        -0.6799546584538376,
        -0.4811324962143237,
        0.5533291818922067
      ],
      "focal" : [
        1112.3385819524738,
        -1112.3385819524738
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam5",
      "origin" : [
        -0.18794969382061746,
        0.15050778588116817,
        0.22565974160643845
      ],
      "principal" : [
        1674.663924414543,
        1107.0629918645168
      ],
      "right" : [
        -0.8063125755443874,
        -0.16222919280057954,
        -0.5688072780144862
      ],
      "up" : [
        0.14008784984055514,
        0.8819152765470074,
        -0.45011202974377895
      ],
      "forward" : [
        -0.57466113914726,
        0.4426139785369265,
        0.6883730392437566
      ],
      "focal" : [
        1116.3177780798248,
        -1116.3177780798248
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam6",
      "origin" : [
        0.30199660239268605,
        0.12945731811209776,
        -0.030640739718502303
      ],
      "principal" : [
        1667.402683243432,
        1071.6938756788727
      ],
      "right" : [
        -0.3835618089078801,
        0.9233322890719002,
        -0.018379953878063238
      ],
      "up" : [
        -0.09052296180386682,
        -0.05739560929358656,
        -0.9942390745791848
      ],
      "forward" : [
        0.9190679692675457,
        0.3796883300696282,
        -0.10559753725995069
      ],
      "focal" : [
        1114.5254166276256,
        -1114.5254166276256
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam7",
      "origin" : [
        0.03075801695822721,
        0.030685088479373363,
        0.3271274518254475
      ],
      "principal" : [
        1674.9024636949234,
        1091.0967408851204
      ],
      "right" : [
        -0.4283114636296572,
        -0.8958904449633681,
        0.11802372959180332
      ],
      "up" : [
        -0.8989551066691598,
        0.4357151544929899,
        0.04507793627252417
      ],
      "forward" : [
        0.09180961995815345,
        0.08679063756240236,
        0.9919870860624402
      ],
      "focal" : [
        1114.8262722394863,
        -1114.8262722394863
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam8",
      "origin" : [
        0.09021288689187959,
        0.2676001283764617,
        -0.17073900061653793
      ],
      "principal" : [
        1673.2718408790158,
        1062.362423709777
      ],
      "right" : [
        -0.9420717258105774,
        0.088747007563084,
        -0.32345762021766955
      ],
      "up" : [
        0.21198887219775575,
        -0.5897868398733404,
        -0.7792382187601826
      ],
      "forward" : [
        0.25992610775488445,
        0.8026677096786793,
        -0.5368081280556207
      ],
      "focal" : [
        1112.3696282398244,
        -1112.3696282398244
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam9",
      "origin" : [
        -0.025506756893975406,
        -0.3277763506511463,
        -0.028496829763474025
      ],
      "principal" : [
        1693.2145502550145,
        1076.5162820964015
      ],
      "right" : [
        -0.25023750899795344,
        0.11319418098366446,
        -0.9615447293194096
      ],
      "up" : [
        -0.9643855564511394,
        0.058750938963071264,
        0.257893050855163
      ],
      "forward" : [
        -0.0856836483754363,
        -0.9918343634712375,
        -0.0944611445975524
      ],
      "focal" : [
        1113.9785962888402,
        -1113.9785962888402
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam10",
      "origin" : [
        -0.13748208149687463,
        0.10550345273210501,
        -0.2808339344325953
      ],
      "principal" : [
        1653.5369511534366,
        1063.6837723020615
      ],
      "right" : [
        -0.8958348211987802,
        0.02950064859577599,
        0.4434069066446507
      ],
      "up" : [
        -0.1607097810509177,
        -0.9517667540643298,
        -0.2613664326810458
      ],
      "forward" : [
        -0.4143094729816294,
        0.3054009783715142,
        -0.8573668427268494
      ],
      "focal" : [
        1112.8451876872618,
        -1112.8451876872618
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam11",
      "origin" : [
        -0.3151298474436943,
        0.09205576614366534,
        -0.03345018938386982
      ],
      "principal" : [
        1687.5913809452898,
        1054.8875601995976
      ],
      "right" : [
        -0.256384669312114,
        -0.9647021842918637,
        -0.06013814899234049
      ],
      "up" : [
        0.12219294110587287,
        0.029369644767298198,
        -0.9920717257890879
      ],
      "forward" : [
        -0.9588199969158,
        0.2617004386483707,
        -0.11034987052840461
      ],
      "focal" : [
        1113.108313299742,
        -1113.108313299742
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam12",
      "origin" : [
        0.06802610706515465,
        -0.23977444744039098,
        0.2162883795126666
      ],
      "principal" : [
        1666.8532697947912,
        1047.510658880164
      ],
      "right" : [
        -0.04596375622760562,
        0.6386319756711976,
        0.768138355222386
      ],
      "up" : [
        0.9791406618508318,
        0.18115150406974256,
        -0.09202008956518848
      ],
      "forward" : [
        0.19791638998265026,
        -0.7478859085606395,
        0.6336369389111263
      ],
      "focal" : [
        1113.9406190637912,
        -1113.9406190637912
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam13",
      "origin" : [
        0.11750513326901402,
        0.2618086387834464,
        0.16293796461779603
      ],
      "principal" : [
        1678.171531839059,
        1040.951677664842
      ],
      "right" : [
        -0.7263706160629491,
        0.5656794887972303,
        -0.3903747482540798
      ],
      "up" : [
        0.5871386794795486,
        0.2154497389804504,
        -0.7802881397485597
      ],
      "forward" : [
        0.35728685839160657,
        0.7959824909678355,
        0.4886286676949031
      ],
      "focal" : [
        1115.326423324583,
        -1115.326423324583
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam14",
      "origin" : [
        -0.1220684113558378,
        0.3065132947601654,
        -0.006993074025770932
      ],
      "principal" : [
        1653.2877266427138,
        1071.5651892414794
      ],
      "right" : [
        -0.6798133925486747,
        -0.26446648001551676,
        0.6840403732672975
      ],
      "up" : [
        -0.6230653802706605,
        -0.28370706239194243,
        -0.7289031723467196
      ],
      "forward" : [
        -0.38683754111983326,
        0.9217200137227881,
        -0.02808795263497399
      ],
      "focal" : [
        1114.388084669443,
        -1114.388084669443
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    },
    {
      "fov" : 1.57079632679,
      "id" : "cam15",
      "origin" : [
        0.2723135680022779,
        -0.03458231402505129,
        0.1831649099542309
      ],
      "principal" : [
        1642.4484290930911,
        1069.064676213803
      ],
      "right" : [
        -0.5235142812818712,
        -0.5327941256427875,
        0.6648783475000997
      ],
      "up" : [
        -0.21222796527630255,
        0.8373061657076325,
        0.503862754748417
      ],
      "forward" : [
        0.8251618556674845,
        -0.12267356907060467,
        0.5514155487496459
      ],
      "focal" : [
        1110.9455768899816,
        -1110.9455768899816
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "distortion" : [
        -0.03413328161902581,
        0.0004374554953464843,
        -0.0018843963208481174
      ],
      "version" : 1
    }
  ]
})";
