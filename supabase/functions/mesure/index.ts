import { createClient } from "https://esm.sh/@supabase/supabase-js@2";

const corsHeaders = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type",
};

interface MesurePayload {
  balise_id: string;
  distance_cm: number;
  batterie_v?: number | null;
  rssi?: number | null;
}

async function sha256Hex(text: string): Promise<string> {
  const data = new TextEncoder().encode(text);
  const hashBuffer = await crypto.subtle.digest("SHA-256", data);
  const hashArray = Array.from(new Uint8Array(hashBuffer));
  return hashArray.map((b) => b.toString(16).padStart(2, "0")).join("");
}

function validatePayload(body: any): { valid: boolean; error?: string; data?: MesurePayload } {
  if (typeof body !== "object" || body === null || Array.isArray(body)) {
    return { valid: false, error: "Le corps doit être un objet JSON" };
  }

  // additionalProperties: false
  const allowedKeys = new Set(["balise_id", "distance_cm", "batterie_v", "rssi"]);
  for (const key of Object.keys(body)) {
    if (!allowedKeys.has(key)) {
      return { valid: false, error: `Champ inattendu: ${key}` };
    }
  }

  // balise_id (pattern: ^bal-[0-9]{2}$)
  if (typeof body.balise_id !== "string" || !/^bal-[0-9]{2}$/.test(body.balise_id)) {
    return { valid: false, error: "balise_id doit être au format ^bal-[0-9]{2}$ (ex: bal-01)" };
  }

  // distance_cm (nombre entre 20 et 600 cm)
  if (
    typeof body.distance_cm !== "number" ||
    Number.isNaN(body.distance_cm) ||
    body.distance_cm < 20 ||
    body.distance_cm > 600
  ) {
    return { valid: false, error: "distance_cm doit être un nombre compris entre 20 et 600" };
  }

  // batterie_v (nombre entre 2.5 et 5.5 ou null)
  if (body.batterie_v !== undefined && body.batterie_v !== null) {
    if (
      typeof body.batterie_v !== "number" ||
      Number.isNaN(body.batterie_v) ||
      body.batterie_v < 2.5 ||
      body.batterie_v > 5.5
    ) {
      return { valid: false, error: "batterie_v doit être comprise entre 2.5 et 5.5 V" };
    }
  }

  // rssi (entier entre -120 et 0 dBm ou null)
  if (body.rssi !== undefined && body.rssi !== null) {
    if (
      typeof body.rssi !== "number" ||
      !Number.isInteger(body.rssi) ||
      body.rssi < -120 ||
      body.rssi > 0
    ) {
      return { valid: false, error: "rssi doit être un entier compris entre -120 et 0 dBm" };
    }
  }

  return {
    valid: true,
    data: {
      balise_id: body.balise_id,
      distance_cm: body.distance_cm,
      batterie_v: body.batterie_v ?? null,
      rssi: body.rssi ?? null,
    },
  };
}

Deno.serve(async (req: Request) => {
  if (req.method === "OPTIONS") {
    return new Response("ok", { headers: corsHeaders });
  }

  if (req.method !== "POST") {
    return new Response(JSON.stringify({ error: "Méthode non autorisée (POST requis)" }), {
      status: 405,
      headers: { ...corsHeaders, "Content-Type": "application/json" },
    });
  }

  // 1. Vérification de l'en-tête Authorization
  const authHeader = req.headers.get("Authorization") || req.headers.get("authorization");
  if (!authHeader || !authHeader.startsWith("Bearer ")) {
    return new Response(JSON.stringify({ error: "En-tête Authorization: Bearer <clé> manquant" }), {
      status: 401,
      headers: { ...corsHeaders, "Content-Type": "application/json" },
    });
  }

  const rawToken = authHeader.slice(7).trim();
  if (!rawToken) {
    return new Response(JSON.stringify({ error: "Clé de balise vide" }), {
      status: 401,
      headers: { ...corsHeaders, "Content-Type": "application/json" },
    });
  }

  // 2. Parser et valider le corps JSON
  let rawBody: any;
  try {
    rawBody = await req.json();
  } catch {
    return new Response(JSON.stringify({ error: "Corps JSON invalide" }), {
      status: 400,
      headers: { ...corsHeaders, "Content-Type": "application/json" },
    });
  }

  const validation = validatePayload(rawBody);
  if (!validation.valid || !validation.data) {
    return new Response(JSON.stringify({ error: validation.error }), {
      status: 400,
      headers: { ...corsHeaders, "Content-Type": "application/json" },
    });
  }

  const payload = validation.data;

  // 3. Client Supabase avec clé service_role
  const supabaseUrl = Deno.env.get("SUPABASE_URL");
  const supabaseServiceKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");

  if (!supabaseUrl || !supabaseServiceKey) {
    console.error("SUPABASE_URL ou SUPABASE_SERVICE_ROLE_KEY manquante");
    return new Response(JSON.stringify({ error: "Erreur serveur de configuration" }), {
      status: 500,
      headers: { ...corsHeaders, "Content-Type": "application/json" },
    });
  }

  const supabase = createClient(supabaseUrl, supabaseServiceKey);

  // 4. Authentification de la balise par SHA-256
  const tokenHash = await sha256Hex(rawToken);

  const { data: balise, error: baliseError } = await supabase
    .from("balises")
    .select("id, cle_api_hash")
    .eq("id", payload.balise_id)
    .single();

  if (baliseError || !balise || balise.cle_api_hash !== tokenHash) {
    return new Response(
      JSON.stringify({ error: "Clé d'API invalide ou non autorisée pour cette balise" }),
      {
        status: 401,
        headers: { ...corsHeaders, "Content-Type": "application/json" },
      }
    );
  }

  // 5. Rate limit : au maximum 1 mesure par seconde
  const { data: lastMeasure, error: lastMeasureError } = await supabase
    .from("mesures")
    .select("ts")
    .eq("balise_id", payload.balise_id)
    .order("ts", { ascending: false })
    .limit(1)
    .maybeSingle();

  if (!lastMeasureError && lastMeasure?.ts) {
    const elapsedMs = Date.now() - new Date(lastMeasure.ts).getTime();
    if (elapsedMs < 1000) {
      return new Response(
        JSON.stringify({ error: "Limite de fréquence dépassée (1 mesure par seconde max)" }),
        {
          status: 429,
          headers: { ...corsHeaders, "Content-Type": "application/json", "Retry-After": "1" },
        }
      );
    }
  }

  // 6. Insérer la mesure
  const { data: inserted, error: insertError } = await supabase
    .from("mesures")
    .insert({
      balise_id: payload.balise_id,
      distance_cm: payload.distance_cm,
      batterie_v: payload.batterie_v,
      rssi: payload.rssi,
    })
    .select()
    .single();

  if (insertError) {
    console.error("Erreur insertion:", insertError);
    return new Response(JSON.stringify({ error: "Erreur lors de l'enregistrement de la mesure" }), {
      status: 500,
      headers: { ...corsHeaders, "Content-Type": "application/json" },
    });
  }

  return new Response(
    JSON.stringify({
      message: "Mesure enregistrée",
      data: inserted,
    }),
    {
      status: 201,
      headers: { ...corsHeaders, "Content-Type": "application/json" },
    }
  );
});
