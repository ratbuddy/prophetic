"""Performance Editor assign list (Parameter Guide 6.4): PE parameter number -> program parameter id
(ExID << 12 | id). Public documentation, no firmware data. Model blocks: STD1/COMB1/VPM1 = OSC1,
STD2/COMB2/VPM2 = OSC2, MOD = OSC2 (the Modulation oscillator is only in OSC2), BRASS/REED/PLUCK = OSC1.
"""
X1, X2 = 1 << 12, 2 << 12


def _build():
    a = [0] * 256
    def put(start, ids):
        for k, i in enumerate(ids):
            a[start + k] = i
    put(1, [155, 156, 157, 158, 159, 160, 162, 163])                   # Pitch EG
    put(9, [173, 174, 175])                                            # portamento
    put(12, [176, 177, 178, 179, 185, 189])                            # OSC1
    put(18, [190, 191, 192, 193, 199, 203])                            # OSC2
    put(24, [205, 206, 207, 208])                                      # sub osc, noise
    put(28, [271, 277, 279, 281, 282, 284])                            # filter 1
    put(34, [287, 293, 295, 297, 298, 300])                            # filter 2
    put(40, [301, 309, 310, 318])                                      # amp 1/2
    put(44, [319, 320, 321, 322, 323, 324, 325, 326, 332, 333, 334, 335, 336])   # amp EG
    put(57, [337, 340, 342, 345, 351])                                 # distortion, wah
    put(62, [356, 358, 361, 364, 365, 366, 367, 371, 372, 373, 382])   # chorus, delay, reverb, pan
    for e in range(4):                                                 # EG1..EG4
        b = 30 + 18 * e
        put(73 + 14 * e, [b, b + 1, b + 2, b + 3, b + 4, b + 5, b + 6, b + 7, b + 8, b + 13, b + 14, b + 15, b + 16, b + 17])
    for l in range(4):                                                 # LFO1..LFO4
        b = 102 + 13 * l
        put(129 + 5 * l, [b, b + 3, b + 8, b + 10, b + 12])
    put(149, [x for k in range(10) for x in (238 + 3 * k, 240 + 3 * k)])   # mixer level, level mod int
    put(169, [210, 212, 218, 219, 221, 222, 223])                      # wave shape 1
    put(176, [224, 226, 232, 233, 235, 236, 237])                      # wave shape 2
    put(183, [23, 24, 25, 26, 27, 28])                                 # common
    std = [388, 390, 391, 392, 394, 396]
    put(189, [X1 | i for i in std])
    put(195, [X2 | i for i in std])
    comb = [397, 399, 401, 403, 405, 406]
    put(201, [X1 | i for i in comb])
    put(207, [X2 | i for i in comb])
    vpm = [407, 408, 410, 412, 413, 415, 417, 418, 423, 424, 425, 427, 430]
    put(213, [X1 | i for i in vpm])
    put(226, [X2 | i for i in vpm])
    put(239, [X2 | i for i in (433, 434, 435, 440)])                   # MOD
    put(243, [X1 | i for i in (447, 456, 462, 463, 464)])              # BRASS
    put(248, [X1 | i for i in (482, 492, 496)])                        # REED (Noise Level = 496, see gen_params.py)
    put(251, [X1 | i for i in (514, 517, 525, 529, 533)])              # PLUCK
    return a


ASSIGN = _build()
