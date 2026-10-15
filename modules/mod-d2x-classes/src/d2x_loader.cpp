// The module folder mod-d2x-classes makes AzerothCore call Addmod_d2x_classesScripts().

void AddSC_d2x_player();
void AddSC_d2x_minions();
void AddSC_d2x_necromancer_spells();
void AddSC_d2x_sorceress_spells();
void AddSC_d2x_shared_spells();
void AddSC_d2x_compatibility();

void Addmod_d2x_classesScripts()
{
    AddSC_d2x_player();
    AddSC_d2x_minions();
    AddSC_d2x_necromancer_spells();
    AddSC_d2x_sorceress_spells();
    AddSC_d2x_shared_spells();
    AddSC_d2x_compatibility();
}
