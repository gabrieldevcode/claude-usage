// =====================================================================
// Clawd CYD — carcaça para a CYD ESP32-2432S028 no formato do Clawd
// =====================================================================
//
// Duas peças, encaixe por pressão, SEM parafuso:
//
//   corpo  — o bichinho. A cara já vem com a janela da tela. A placa entra
//            por TRÁS, tela virada para a janela.
//   tampa  — fecha as costas, prende a placa e leva o canal do cabo.
//
// Por que a cara é inteiriça e a abertura fica atrás: assim não há emenda
// nenhuma na frente, que é o lado que se olha.
//
// ---------------------------------------------------------------------
// DE ONDE VÊM AS MEDIDAS — leia antes de imprimir a peça inteira
// ---------------------------------------------------------------------
// As medidas da placa são as PUBLICADAS para a ESP32-2432S028, não medidas
// nesta unidade com paquímetro:
//
//   PCB                86 x 50 x 1,6 mm
//   módulo da tela     70 x 50 mm, sobressaindo 4 mm da face do PCB
//   área visível       59,5 x 45 mm
//   componentes atrás  até 4,8 mm (botões e conector do alto-falante)
//
// Duas coisas eu NÃO encontrei publicadas e portanto NÃO uso:
//   - a posição dos furos de fixação  -> a placa é presa por abas elásticas
//     nas bordas, e o modelo não tem furo nenhum;
//   - a posição exata do conector USB -> a abertura ocupa quase toda a
//     lateral esquerda, então acerta o conector onde quer que ele esteja.
//
// A posição da área visível dentro do módulo também não é publicada. Em vez
// de supor, a janela expõe o módulo quase inteiro (67 x 47) e o rebaixo
// segura a borda dele. Assim a área visível cai dentro da janela em qualquer
// posição plausível.
//
// >>> IMPRIMA A PEÇA "teste" ANTES. São ~10 minutos e 3 g de filamento, e
// >>> ela verifica o encaixe do PCB e a abertura do USB sem comprometer
// >>> 6 horas de impressão da carcaça.
//
// ---------------------------------------------------------------------
// COMO GERAR OS STL
// ---------------------------------------------------------------------
//   openscad -o corpo.stl -D "part=\"corpo\"" clawd_cyd.scad
//   openscad -o tampa.stl -D "part=\"tampa\"" clawd_cyd.scad
//   openscad -o teste.stl -D "part=\"teste\"" clawd_cyd.scad
//
// (ou rode ./build.sh nesta pasta)
//
// ---------------------------------------------------------------------
// COMO IMPRIMIR
// ---------------------------------------------------------------------
// As duas peças foram desenhadas para imprimir DEITADAS, sem suporte nenhum:
// todo vão para cima é chanfrado a 45°, que é auto-sustentável.
//
//   corpo  cara para BAIXO na mesa (a cavidade abre para cima)
//   tampa  face lisa para BAIXO
//
//   bico 0,4 mm · camada 0,2 mm · 3 perímetros · 15% de preenchimento
//   PLA ou PETG · SEM suporte · SEM balsa
//
// PETG deixa as abas mais tolerantes; PLA imprime melhor os detalhes da cara.

// =====================================================================
// Parâmetros
// =====================================================================
part = "corpo";          // "corpo" | "tampa" | "teste" | "montado"

/* [Placa] */
pcb_w      = 86.0;       // comprimento do PCB
pcb_h      = 50.0;       // largura do PCB
pcb_t      = 1.6;        // espessura do PCB
disp_w     = 70.0;       // módulo da tela
disp_h     = 50.0;
disp_z     = 4.0;        // quanto o módulo sobressai da face do PCB
back_comp  = 5.2;        // altura livre para os componentes de trás (4,8 + folga)

/* [Folgas] */
fit        = 0.4;        // folga por lado do PCB. Aumente para 0,6 se ficar apertado
wall       = 2.4;        // parede externa
face       = 2.2;        // espessura da cara (frente)
lid_t      = 2.0;        // espessura da tampa

/* [Silhueta do Clawd] */
side_pad   = 9.0;        // borda dos lados
top_pad    = 19.0;       // borda de cima — é onde moram os olhos. Precisa ser
                         // generosa: com 14 mm os olhos viravam dois pontinhos
                         // espremidos e a peça lia como televisão, não bicho
bot_pad    = 9.0;        // borda de baixo
corner_r   = 14.0;       // cantos bem redondos, como no pixel art
leg_n      = 4;          // perninhas
leg_w      = 9.0;
leg_h      = 8.0;
leg_r      = 3.0;

/* [Cara] */
eye_w      = 8.0;        // olhos: retângulos verticais, como no pixel art
eye_h      = 11.0;
eye_gap    = 40.0;       // distância entre os centros
eye_deep   = 1.4;        // profundidade do rebaixo

/* [Encaixes] */
tab_n      = 4;          // abas que prendem o PCB pela frente
tab_w      = 7.0;
tab_grip   = 1.4;        // avanço da aba a partir da parede. Como a folga fit
                         // deixa o PCB a 0,4 mm dela, a pega real é 1,0 mm -
                         // suficiente, e mantém o vão de baixo imprimível
clip_w     = 10.0;       // clipes da tampa
clip_deep  = 0.9;        // profundidade do ressalto
clip_post  = 2.2;        // altura da haste do clipe
clip_barb  = 1.5;        // altura do ressalto
lid_gap    = 0.35;       // folga da tampa dentro do rebaixo
lid_flange = 3.0;        // quanto a tampa avança além da cavidade

/* [Cabo] */
usb_open_w = 26.0;       // abertura do USB na lateral esquerda
cable_w    = 9.0;        // canal do cabo, saindo por trás
cable_h    = 6.0;

$fn = 48;

// =====================================================================
// Derivadas
// =====================================================================
cav_w  = pcb_w + 2 * fit;                 // cavidade do PCB
cav_h  = pcb_h + 2 * fit;
body_w = pcb_w + 2 * (fit + wall + side_pad);
body_h = pcb_h + 2 * fit + wall * 2 + top_pad + bot_pad;

// deslocamento vertical: a borda de cima é maior que a de baixo, então o
// centro do PCB não é o centro do corpo
pcb_dy = (bot_pad - top_pad) / 2;

// profundidade interna: componentes de trás + PCB + módulo da tela
depth_in = back_comp + pcb_t + disp_z;
body_z   = face + depth_in + lid_t;

win_w = disp_w - 3.0;                     // janela: expõe o módulo menos o rebaixo
win_h = disp_h - 3.0;

// Faixa em Z da abertura do USB: começa um pouco antes da face frontal do PCB
// e vai até a tampa. O conector fica do lado dos componentes, não do lado da
// tela, então centrar a abertura no PCB deixaria metade dela inútil.
usb_z0 = face + disp_z - 0.6;
usb_z1 = body_z - lid_t;

// =====================================================================
// Primitivas
// =====================================================================

// retângulo 2D de cantos arredondados
module rrect(w, h, r) {
    offset(r = r) offset(r = -r) square([w, h], center = true);
}

// silhueta do bichinho: corpo arredondado + perninhas embaixo
module clawd_2d() {
    union() {
        translate([0, 0]) rrect(body_w, body_h, corner_r);
        // perninhas, distribuídas na largura útil do corpo
        span = body_w - 2 * corner_r - leg_w;
        for (i = [0 : leg_n - 1]) {
            x = -span / 2 + span * i / (leg_n - 1);
            translate([x, -body_h / 2 - leg_h / 2 + leg_r])
                rrect(leg_w, leg_h + leg_r * 2, leg_r);
        }
    }
}

// prisma com o topo chanfrado a 45°, para vãos ficarem auto-sustentáveis
module chamfered_slot(w, h, z, cham) {
    hull() {
        translate([0, 0, 0])        linear_extrude(0.01) square([w, h], center = true);
        translate([0, 0, cham])     linear_extrude(0.01)
            square([w + 2 * cham, h + 2 * cham], center = true);
        translate([0, 0, z - 0.01]) linear_extrude(0.01)
            square([w + 2 * cham, h + 2 * cham], center = true);
    }
}

// =====================================================================
// CORPO — a cara com a janela, e a cavidade abrindo para trás
// =====================================================================
// Z = 0 é a face da frente (a que encosta na mesa da impressora).
// Z cresce para trás.
module corpo() {
    difference() {
        // ---- volume cheio ----
        linear_extrude(body_z) clawd_2d();

        // ---- janela da tela, com rebaixo que segura o módulo ----
        // A janela é menor que o módulo; o degrau entre as duas é chanfrado a
        // 45° para imprimir sem ponte.
        translate([0, pcb_dy, -0.1])
            linear_extrude(face + 0.2) rrect(win_w, win_h, 2.5);
        translate([0, pcb_dy, face - 0.01])
            chamfered_slot(win_w, win_h, 1.6, 1.5);

        // ---- alojamento do módulo da tela ----
        translate([0, pcb_dy, face + 1.5 - 0.01])
            linear_extrude(disp_z + 0.6)
                square([disp_w + 2 * fit, disp_h + 2 * fit], center = true);

        // ---- cavidade do PCB e dos componentes de trás ----
        translate([0, pcb_dy, face + disp_z])
            linear_extrude(body_z) square([cav_w, cav_h], center = true);

        // ---- abertura do USB, na lateral esquerda ----
        // Larga de propósito: a posição exata do conector ao longo da borda não
        // é publicada, então a abertura cobre 26 mm e acerta onde quer que ele
        // esteja. Em Z ela vai da face frontal do PCB até a tampa, porque o
        // conector é soldado no PCB e cresce para o lado dos componentes.
        translate([-(cav_w / 2 + (wall + side_pad) / 2 + 1),
                   pcb_dy,
                   (usb_z0 + usb_z1) / 2])
            cube([wall + side_pad + 4, usb_open_w, usb_z1 - usb_z0], center = true);

        // ---- olhos, rebaixados na borda de cima ----
        for (sx = [-1, 1])
            translate([sx * eye_gap / 2, body_h / 2 - top_pad / 2 + pcb_dy, -0.01])
                linear_extrude(eye_deep) rrect(eye_w, eye_h, 1.2);

        // ---- rebaixo em que a tampa afunda, para ficar rente às costas ----
        translate([0, pcb_dy, body_z - lid_t])
            linear_extrude(lid_t + 0.1)
                rrect(cav_w + 2 * lid_flange, cav_h + 2 * lid_flange, 3);

        // ---- entalhes onde os clipes da tampa travam ----
        for (sx = [-1, 1])
            translate([sx * (cav_w / 2 + clip_deep / 2), pcb_dy,
                       body_z - lid_t - clip_post - clip_barb / 2])
                cube([clip_deep + 0.3, clip_w + 0.4, clip_barb + 0.2], center = true);
    }

    // ---- abas que prendem o PCB pela frente ----
    // Ficam nas bordas de cima e de baixo, onde o módulo termina; avançam
    // tab_grip sobre a face do PCB e são chanfradas para a placa entrar
    // empurrando.
    for (sy = [-1, 1])
        for (i = [-1, 1])
            translate([i * (pcb_w / 4), sy * (cav_h / 2) + pcb_dy, face + disp_z + pcb_t])
                aba(sy);
}

// Aba de retenção do PCB.
//
// Duas exigências que brigam entre si, e como elas se resolvem:
//
//   A placa entra POR TRÁS (de Z maior para Z menor) e precisa achar uma rampa
//   nesse caminho, senão bate no ponto mais saliente e não passa. Logo o lado
//   de CIMA da aba tem de ser inclinado.
//
//   Mas a peça imprime de Z=0 para cima, e a aba fica a 7,8 mm de altura, no
//   meio da impressão. Se ela nascesse afastada da parede — que é o que uma
//   rampa simples de baixo para cima faria — o bico depositaria material no ar.
//
// A forma que atende as duas: um ressalto ancorado na parede desde a primeira
// camada, com a face de baixo plana (é ela que trava a placa) e chanfro de 45°
// só no topo (é ele a rampa de entrada). O vão de baixo são 1,4 mm saindo da
// parede — dentro do que qualquer impressora faz em balanço.
module aba(sy) {
    hull() {
        // corpo do ressalto: face de baixo plana, encostada na parede
        translate([0, sy * (1.0 - tab_grip / 2), 0.5])
            cube([tab_w, tab_grip + 2.0, 1.0], center = true);
        // topo recuando para a parede: a rampa de entrada, a 45°
        translate([0, sy * 1.4, 1.0 + tab_grip])
            cube([tab_w, 1.2, 0.01], center = true);
    }
}

// =====================================================================
// TAMPA — fecha as costas e leva o canal do cabo
// =====================================================================
// A tampa NÃO repete a silhueta inteira do bichinho: ela tem o tamanho do
// rebaixo aberto nas costas do corpo, e afunda nele até ficar rente. Uma tampa
// com a silhueta toda ficaria pousada por cima, somando 2 mm à espessura e
// deixando as perninhas duplicadas em duas peças que precisariam alinhar.
//
// Impressão: face lisa para baixo. Os clipes crescem para cima e têm chanfro
// de 45° nas duas pontas, então entram empurrando e travam no entalhe.
module tampa() {
    lw = cav_w + 2 * lid_flange - lid_gap;
    lh = cav_h + 2 * lid_flange - lid_gap;
    difference() {
        union() {
            linear_extrude(lid_t) rrect(lw, lh, 3);
            // saia que entra na cavidade e encosta na traseira do PCB
            translate([0, 0, lid_t - 0.01])
                linear_extrude(1.2)
                    square([cav_w - 0.6, cav_h - 0.6], center = true);
        }
        // canal do cabo, saindo por trás em vez de pela lateral: assim o cabo
        // dobra atrás do bichinho e não fica aparecendo do lado
        translate([-lw / 2 + 8, 0, -0.1])
            linear_extrude(lid_t + 2) rrect(cable_w, cable_h, 2);
    }

    for (sx = [-1, 1])
        translate([sx * (cav_w / 2 - 0.8), 0, lid_t]) clipe(sx);
}

module clipe(sx) {
    union() {
        translate([0, 0, clip_post / 2])
            cube([1.6, clip_w, clip_post], center = true);
        // Ressalto em losango: a face de baixo é a rampa de entrada e a de
        // cima é a que trava. Ambas a 45°, então imprime sem suporte de pé.
        translate([sx * (0.8 + clip_deep / 2), 0, clip_post + clip_barb / 2])
            hull() {
                translate([0, 0, -clip_barb / 2])
                    cube([0.01, clip_w, 0.01], center = true);
                cube([clip_deep, clip_w, 0.01], center = true);
                translate([0, 0, clip_barb / 2])
                    cube([0.01, clip_w, 0.01], center = true);
            }
    }
}

// =====================================================================
// TESTE DE ENCAIXE — só o essencial, para conferir antes da peça inteira
// =====================================================================
// Um anel de 10 mm que reproduz a cavidade do PCB, uma aba de cada lado e a
// abertura do USB. Se a placa entra e trava aqui, entra na carcaça.
module teste() {
    h = 10;
    difference() {
        linear_extrude(h) rrect(cav_w + 2 * wall, cav_h + 2 * wall, 3);
        translate([0, 0, -0.1]) linear_extrude(h + 0.2)
            square([cav_w, cav_h], center = true);
        translate([-cav_w / 2 - wall - 1, 0, 2])
            rotate([0, 90, 0]) linear_extrude(wall + 2)
                rrect(pcb_t + back_comp - 1.2, usb_open_w, 1.5);
    }
    for (sy = [-1, 1])
        translate([0, sy * (cav_h / 2), 2 + pcb_t]) aba(sy);
    // degrau em que o PCB apoia
    for (sy = [-1, 1])
        translate([0, sy * (cav_h / 2 + 0.6), 1])
            cube([cav_w - 20, 1.2, 2], center = true);
}

// =====================================================================
// Seleção da peça
// =====================================================================
if (part == "corpo")      corpo();
else if (part == "tampa") tampa();
else if (part == "teste") teste();
else if (part == "montado") {
    corpo();
    translate([0, 0, body_z + 12]) rotate([180, 0, 0]) tampa();
}
