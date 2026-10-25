"""
train_passenger_car_tbusml.py
Treina uma rede neural TinyML para veículo de passeio e exporta para .tbusml
"""

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim

# 1. Definição da Arquitetura (6 Entradas -> 10 ReLU -> 8 Tanh -> 7 Linear)
class PassengerCarTinyML(nn.Module):

    def __init__(self):
        super().__init__()
        self.layer1 = nn.Linear(6, 10)
        self.act1 = nn.ReLU()
        self.layer2 = nn.Linear(10, 8)
        self.act2 = nn.Tanh()
        self.head = nn.Linear(8, 7)

    def forward(self, x):
        h1 = self.act1(self.layer1(x))
        h2 = self.act2(self.layer2(h1))
        return self.head(h2)

# 2. Geração de Dados Sintéticos de Condução Automotiva de Passeio
def generate_dataset(num_samples=15000):

    np.random.seed(42)

    X = []
    y_regime = []
    y_anomaly = []
    y_health = []

    for _ in range(num_samples):

        r = np.random.rand()

        if r < 0.25:  # Regime 0: Idle (Marcha lenta / parado no semáforo)
            speed = np.random.uniform(0.0, 5.0)
            accel = np.random.uniform(-0.5, 0.5)
            temp = np.random.uniform(70.0, 85.0)
            delta = temp - 75.0
            jitter = np.random.uniform(0.01, 0.05)
            stress = 0.0
            label = 0
            anomaly_score = 0.02
            health = 0.98

        elif r < 0.60:  # Regime 1: Cruise (Cruzeiro em pista plana / rodovia)
            speed = np.random.uniform(40.0, 110.0)
            accel = np.random.uniform(-1.0, 1.5)
            expected_temp = 75.0 + 0.12 * speed
            temp = expected_temp + np.random.normal(0, 2.0)
            delta = temp - expected_temp
            jitter = np.random.uniform(0.02, 0.08)
            stress = 0.0
            label = 1
            anomaly_score = 0.05
            health = 0.95

        elif r < 0.80:  # Regime 2: HighLoad (Ultrapassagem, subida de serra)
            speed = np.random.uniform(30.0, 130.0)
            accel = np.random.uniform(4.0, 12.0)
            expected_temp = 75.0 + 0.12 * speed
            temp = expected_temp + np.random.uniform(3.0, 8.0)
            delta = temp - expected_temp
            jitter = np.random.uniform(0.03, 0.10)
            stress = 0.1
            label = 2
            anomaly_score = 0.15
            health = 0.88

        elif r < 0.90:  # Regime 3: ThermalStress (Superaquecimento / ventoinha falhando)
            speed = np.random.uniform(10.0, 90.0)
            accel = np.random.uniform(-2.0, 2.0)
            temp = np.random.uniform(102.0, 120.0)
            delta = np.random.uniform(15.0, 35.0)
            jitter = np.random.uniform(0.05, 0.15)
            stress = 0.4
            label = 3
            anomaly_score = 0.80
            health = 0.20

        else:  # Regime 4: Anomaly (Injeção de mensagens espúrias ou pane de barramento)
            speed = np.random.uniform(0.0, 150.0)
            accel = np.random.uniform(-20.0, 20.0)
            temp = np.random.uniform(40.0, 130.0)
            delta = np.random.uniform(10.0, 40.0)
            jitter = np.random.uniform(0.30, 1.0)
            stress = np.random.uniform(0.6, 1.5)
            label = 4
            anomaly_score = 0.95
            health = 0.40

        X.append([speed, accel, temp, delta, jitter, stress])
        y_regime.append(label)
        y_anomaly.append(anomaly_score)
        y_health.append(health)

    return np.array(X, dtype=np.float32), np.array(y_regime, dtype=np.int64), \
           np.array(y_anomaly, dtype=np.float32), np.array(y_health, dtype=np.float32)

# 3. Treinamento
X_raw, y_cls, y_anom, y_hlth = generate_dataset()

# Constantes de normalização padrão para carro de passeio
feat_mean = np.array([45.0, 0.0, 80.0, 0.0, 0.0, 0.0], dtype=np.float32)
feat_scale = np.array([1.0/45.0, 1.0/15.0, 1.0/20.0, 1.0/12.0, 1.0, 1.0], dtype=np.float32)

X_norm = np.clip((X_raw - feat_mean) * feat_scale, -6.0, 6.0)

t_X = torch.tensor(X_norm)
t_y_cls = torch.tensor(y_cls)
t_y_anom = torch.tensor(y_anom).unsqueeze(1)
t_y_hlth = torch.tensor(y_hlth).unsqueeze(1)

model = PassengerCarTinyML()
criterion_cls = nn.CrossEntropyLoss()
criterion_reg = nn.BCEWithLogitsLoss()
optimizer = optim.Adam(model.parameters(), lr=0.005)

print("Treinando modelo TinyML automotivo...")
for epoch in range(120):
    optimizer.zero_grad()
    out = model(t_X)
    loss_cls = criterion_cls(out[:, 0:5], t_y_cls)
    loss_anom = criterion_reg(out[:, 5:6], t_y_anom)
    loss_hlth = criterion_reg(out[:, 6:7], t_y_hlth)
    loss = loss_cls + 2.0 * loss_anom + 2.0 * loss_hlth
    loss.backward()
    optimizer.step()

print(f"Treinamento concluído. Loss final: {loss.item():.4f}")

# 4. Quantização Simétrica int8 e Exportação para formato .tbusml
def quantize_layer(weights, biases):
    # Quantização simétrica: max(|W|) mapeado para 127
    max_val = np.max(np.abs(weights))
    scale = float(max_val / 127.0) if max_val > 0 else 0.01
    q_weights = np.clip(np.round(weights / scale), -128, 127).astype(np.int8)
    return q_weights, biases, scale

def export_tbusml(filename, model_name, feat_mean, feat_scale, layers_info):
    with open(filename, 'w') as f:
        f.write("# TorqueBus Studio TinyML Quantized Model - Automotivo de Passeio\n")
        f.write("TBUSML 1\n")
        f.write(f"model_name {model_name}\n")
        f.write(f"feature_mean {' '.join(f'{m:.4f}' for m in feat_mean)}\n")
        f.write(f"feature_scale {' '.join(f'{s:.7f}' for s in feat_scale)}\n\n")

        for name, in_sz, out_sz, act, q_w, b, scale in layers_info:
            f.write(f"layer {name} {in_sz} {out_sz} {act} {scale:.6f}\n")
            for row in q_w:
                f.write(f"weights {' '.join(str(w) for w in row)}\n")
            f.write(f"biases {' '.join(f'{val:.4f}' for val in b)}\n\n")
    print(f"Modelo exportado com sucesso para: {filename}")

# Extrair pesos e quantizar
l1_w = model.layer1.weight.detach().numpy()
l1_b = model.layer1.bias.detach().numpy()
q_l1_w, q_l1_b, s1 = quantize_layer(l1_w, l1_b)

l2_w = model.layer2.weight.detach().numpy()
l2_b = model.layer2.bias.detach().numpy()
q_l2_w, q_l2_b, s2 = quantize_layer(l2_w, l2_b)

lh_w = model.head.weight.detach().numpy()
lh_b = model.head.bias.detach().numpy()
q_lh_w, q_lh_b, sh = quantize_layer(lh_w, lh_b)

layers_info = [
    ("dense_feature_extractor_int8", 6, 10, "relu", q_l1_w, q_l1_b, s1),
    ("dense_regime_embedding_int8", 10, 8, "tanh", q_l2_w, q_l2_b, s2),
    ("dense_output_head_int8", 8, 7, "linear", q_lh_w, q_lh_b, sh),
]

export_tbusml("meu_veiculo_passeio.tbusml", "passenger_car_powertrain_v1", feat_mean, feat_scale, layers_info)
